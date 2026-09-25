/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include "persistent_key_store.h"

#include "persistent_key_backend.h"
#include "persistent_key_persistence.h"

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <algorithm>

LOG_MODULE_REGISTER(aliro_ud_key, CONFIG_ALIRO_UD_KEY_LOG_LEVEL);

using namespace Aliro;
using namespace Aliro::UserDevice;

/*
 * Kpersistent record engine. Key-ID allocation mirrors
 * storage/credential/credential_store.cpp's ping-pong scheme: slot i
 * (0-based) owns two adjacent persistent PSA key IDs starting at
 * CONFIG_ALIRO_UD_PERSISTENT_KEY_ID_BASE, (base + 2*i) and (base + 2*i + 1),
 * so a Replace() can always durably own its new key at a *different* ID
 * than the one currently committed for that slot, making it safe to destroy
 * the old key only after the new one is committed.
 */
namespace AliroUd::PersistentKey::Store {
namespace {

K_MUTEX_DEFINE(sMutex);
std::array<Record, kMaxRecords> sRecords{};
PersistentKeyHandle sNextHandle{ 1 };

class Lock {
public:
	Lock() { k_mutex_lock(&sMutex, K_FOREVER); }
	~Lock() { k_mutex_unlock(&sMutex); }
	Lock(const Lock &) = delete;
	Lock &operator=(const Lock &) = delete;
};

constexpr CryptoTypes::KeyId kKeyIdBase{ CONFIG_ALIRO_UD_PERSISTENT_KEY_ID_BASE };

CryptoTypes::KeyId CommittedKeyId(size_t slotIndex)
{
	return kKeyIdBase + static_cast<CryptoTypes::KeyId>(2 * slotIndex);
}

CryptoTypes::KeyId StagedKeyId(size_t slotIndex)
{
	return kKeyIdBase + static_cast<CryptoTypes::KeyId>(2 * slotIndex) + 1;
}

Record *FindLocked(CredentialHandle handle, const ReaderGroupSubIdentifier &subIdentifier)
{
	for (auto &record : sRecords) {
		if (record.mValid && record.mCredentialHandle == handle &&
		    record.mReaderGroupSubIdentifier == subIdentifier) {
			return &record;
		}
	}
	return nullptr;
}

Record *FindLocked(PersistentKeyHandle handle)
{
	if (handle == kInvalidPersistentKeyHandle) {
		return nullptr;
	}

	for (auto &record : sRecords) {
		if (record.mValid && record.mHandle == handle) {
			return &record;
		}
	}
	return nullptr;
}

size_t CountForCredentialLocked(CredentialHandle handle)
{
	return static_cast<size_t>(std::count_if(sRecords.begin(), sRecords.end(), [handle](const Record &record) {
		return record.mValid && record.mCredentialHandle == handle;
	}));
}

Record *FindFreeSlotLocked()
{
	for (auto &record : sRecords) {
		if (!record.mValid) {
			return &record;
		}
	}
	return nullptr;
}

size_t SlotIndexOf(const Record *record)
{
	return static_cast<size_t>(record - sRecords.data());
}

/*
 * Record handles are never kInvalidPersistentKeyHandle and never alias a
 * live record, even after sNextHandle wraps or Init() restores a persisted
 * handle at the top of the range. At most kMaxRecords handles are live, so
 * the scan terminates.
 */
PersistentKeyHandle NextFreeHandleLocked()
{
	PersistentKeyHandle candidate = sNextHandle;
	while (candidate == kInvalidPersistentKeyHandle || FindLocked(candidate) != nullptr) {
		++candidate;
	}
	return candidate;
}

AliroError PersistLocked(size_t slotIndex, const Record &record)
{
	return Persistence::SaveRecord(slotIndex, record);
}

} // namespace

AliroError Init()
{
	AliroError error = Persistence::Init();
	if (error != ALIRO_NO_ERROR) {
		return error;
	}

	sRecords = {};
	sNextHandle = 1;

	for (size_t i = 0; i < kMaxRecords; ++i) {
		Record loaded{};
		bool present{ false };
		error = Persistence::LoadRecord(i, loaded, present);
		if (error != ALIRO_NO_ERROR) {
			return error;
		}

		if (present) {
			sRecords[i] = loaded;
			if (loaded.mHandle >= sNextHandle) {
				sNextHandle = loaded.mHandle + 1;
			}
		}
	}

	return ALIRO_NO_ERROR;
}

AliroError Lookup(CredentialHandle handle, const ReaderGroupSubIdentifier &readerGroupSubIdentifier,
		   PersistentKeyHandle &outRecord, CryptoTypes::KeyId &outKeyId)
{
	outRecord = kInvalidPersistentKeyHandle;
	outKeyId = 0;
	Lock lock;

	const Record *found = FindLocked(handle, readerGroupSubIdentifier);
	if (found == nullptr) {
		return ALIRO_ERROR_UNKNOWN;
	}

	CryptoTypes::KeyId mintedKeyId{ 0 };
	const AliroError error = Backend::MintVolatileHandle(found->mPersistedKeyId, mintedKeyId);
	if (error != ALIRO_NO_ERROR) {
		return error;
	}

	outRecord = found->mHandle;
	outKeyId = mintedKeyId;
	return ALIRO_NO_ERROR;
}

AliroError Replace(CredentialHandle handle, const ReaderGroupSubIdentifier &readerGroupSubIdentifier,
		    CryptoTypes::KeyId keyId, PersistentKeyHandle &outRecord)
{
	outRecord = kInvalidPersistentKeyHandle;
	if (handle == kInvalidCredentialHandle || keyId == 0) {
		return ALIRO_INVALID_ARGUMENT;
	}

	Lock lock;

	Record *existing = FindLocked(handle, readerGroupSubIdentifier);

	Record *targetSlot = existing;
	if (targetSlot == nullptr) {
		if (CountForCredentialLocked(handle) >= kMaxRecordsPerCredential) {
			return ALIRO_NO_MEMORY;
		}

		targetSlot = FindFreeSlotLocked();
		if (targetSlot == nullptr) {
			return ALIRO_NO_MEMORY;
		}
	}

	const size_t slotIndex = SlotIndexOf(targetSlot);
	const CryptoTypes::KeyId oldPersistedKeyId = existing != nullptr ? existing->mPersistedKeyId : 0;
	const CryptoTypes::KeyId desiredPersistentKeyId =
		(oldPersistedKeyId == CommittedKeyId(slotIndex)) ? StagedKeyId(slotIndex) : CommittedKeyId(slotIndex);

	/*
	 * Durably own the new key material before touching any storage
	 * (in-memory or persisted): on failure here, the old record (if any)
	 * is untouched and remains resolvable by Lookup() with its prior
	 * outKeyId (interface.h's failure-atomicity contract).
	 */
	CryptoTypes::KeyId actualPersistedKeyId{};
	const AliroError ownError = Backend::DurablyOwn(keyId, desiredPersistentKeyId, actualPersistedKeyId);
	if (ownError != ALIRO_NO_ERROR) {
		return ownError;
	}

	Record newRecord{};
	newRecord.mValid = true;
	newRecord.mHandle = (existing != nullptr) ? existing->mHandle : NextFreeHandleLocked();
	newRecord.mCredentialHandle = handle;
	newRecord.mReaderGroupSubIdentifier = readerGroupSubIdentifier;
	newRecord.mPersistedKeyId = actualPersistedKeyId;

	const AliroError persistError = PersistLocked(slotIndex, newRecord);
	if (persistError != ALIRO_NO_ERROR) {
		Backend::Destroy(actualPersistedKeyId);
		return persistError;
	}

	*targetSlot = newRecord;
	if (existing == nullptr) {
		sNextHandle = newRecord.mHandle + 1;
	}

	/* Only now retire the old key: the new record is durably committed. */
	if (oldPersistedKeyId != 0 && oldPersistedKeyId != actualPersistedKeyId) {
		Backend::Destroy(oldPersistedKeyId);
	}

	outRecord = newRecord.mHandle;
	return ALIRO_NO_ERROR;
}

AliroError Delete(PersistentKeyHandle record)
{
	Lock lock;

	Record *found = FindLocked(record);
	if (found == nullptr) {
		return ALIRO_NO_ERROR;
	}

	const size_t slotIndex = SlotIndexOf(found);
	const CryptoTypes::KeyId persistedKeyId = found->mPersistedKeyId;

	*found = Record{};
	const AliroError eraseError = Persistence::EraseRecord(slotIndex);
	if (eraseError != ALIRO_NO_ERROR) {
		return eraseError;
	}

	return Backend::Destroy(persistedKeyId);
}

AliroError Reset()
{
	Lock lock;

	AliroError firstError{ ALIRO_NO_ERROR };

	for (size_t i = 0; i < kMaxRecords; ++i) {
		if (!sRecords[i].mValid) {
			continue;
		}

		const CryptoTypes::KeyId persistedKeyId = sRecords[i].mPersistedKeyId;
		sRecords[i] = Record{};

		AliroError error = Persistence::EraseRecord(i);
		if (error == ALIRO_NO_ERROR) {
			error = Backend::Destroy(persistedKeyId);
		}
		if (error != ALIRO_NO_ERROR && firstError == ALIRO_NO_ERROR) {
			firstError = error;
		}
	}

	return firstError;
}

AliroError DeleteAllForCredential(CredentialHandle handle)
{
	Lock lock;

	AliroError firstError{ ALIRO_NO_ERROR };

	for (size_t i = 0; i < kMaxRecords; ++i) {
		if (!sRecords[i].mValid || sRecords[i].mCredentialHandle != handle) {
			continue;
		}

		const CryptoTypes::KeyId persistedKeyId = sRecords[i].mPersistedKeyId;
		sRecords[i] = Record{};

		AliroError error = Persistence::EraseRecord(i);
		if (error == ALIRO_NO_ERROR) {
			error = Backend::Destroy(persistedKeyId);
		}
		if (error != ALIRO_NO_ERROR && firstError == ALIRO_NO_ERROR) {
			firstError = error;
		}
	}

	return firstError;
}

} // namespace AliroUd::PersistentKey::Store
