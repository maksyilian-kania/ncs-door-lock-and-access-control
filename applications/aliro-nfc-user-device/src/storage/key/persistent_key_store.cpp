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
 *
 * Durable model: slot i's persisted record (one atomically written settings
 * entry) is authoritative, and the only PSA object slot i may own is the
 * key at that record's mPersistedKeyId. The record write is the single
 * commit point of Replace() and the record erase that of every deletion;
 * every interruption therefore leaves either the old or the new record,
 * plus at most unreferenced keys at the slot's two IDs. SweepSlotLocked()
 * destroys those, both at boot and before a Replace() reuses the slot, so
 * a cleanup failure is always retained as recoverable state. Init() also
 * drops records that cannot be authoritative (malformed, duplicate, a key
 * ID outside the slot's pair, or a missing key).
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

void KeepFirstError(AliroError &firstError, AliroError error)
{
	if (error != ALIRO_NO_ERROR && firstError == ALIRO_NO_ERROR) {
		firstError = error;
	}
}

/*
 * Destroys every key present at the slot's two IDs other than `keepKeyId`.
 * Only keys known to be present are destroyed, so repeated sweeps never
 * destroy the same object twice. Visits both IDs and returns the first error.
 */
AliroError SweepSlotLocked(size_t slotIndex, CryptoTypes::KeyId keepKeyId)
{
	AliroError firstError{ ALIRO_NO_ERROR };

	for (const CryptoTypes::KeyId keyId : { CommittedKeyId(slotIndex), StagedKeyId(slotIndex) }) {
		if (keyId == keepKeyId) {
			continue;
		}

		bool present{ false };
		AliroError error = Backend::Exists(keyId, present);
		if (error == ALIRO_NO_ERROR && present) {
			error = Backend::Destroy(keyId);
		}
		KeepFirstError(firstError, error);
	}

	return firstError;
}

/*
 * Erases the slot's record (the commit point), then destroys every key the
 * slot owns. An erase failure leaves the record and its key untouched.
 */
AliroError RemoveSlotLocked(size_t slotIndex)
{
	if (sRecords[slotIndex].mValid) {
		const AliroError eraseError = Persistence::EraseRecord(slotIndex);
		if (eraseError != ALIRO_NO_ERROR) {
			return eraseError;
		}
		sRecords[slotIndex] = Record{};
	}

	return SweepSlotLocked(slotIndex, 0);
}

/*
 * Decides whether a loaded record may become authoritative. Records loaded
 * from lower slots are already in sRecords, so the first of two duplicates
 * wins. A key whose presence cannot be determined keeps the record and
 * reports the error.
 */
AliroError AcceptLoadedLocked(size_t slotIndex, const Record &loaded, bool &outAccept)
{
	outAccept = loaded.mValid && loaded.mHandle != kInvalidPersistentKeyHandle &&
		    loaded.mCredentialHandle != kInvalidCredentialHandle &&
		    (loaded.mPersistedKeyId == CommittedKeyId(slotIndex) ||
		     loaded.mPersistedKeyId == StagedKeyId(slotIndex)) &&
		    FindLocked(loaded.mHandle) == nullptr &&
		    FindLocked(loaded.mCredentialHandle, loaded.mReaderGroupSubIdentifier) == nullptr;
	if (!outAccept) {
		return ALIRO_NO_ERROR;
	}

	bool keyPresent{ false };
	const AliroError error = Backend::Exists(loaded.mPersistedKeyId, keyPresent);
	outAccept = keyPresent || error != ALIRO_NO_ERROR;
	return error;
}

} // namespace

AliroError Init()
{
	AliroError error = Persistence::Init();
	if (error != ALIRO_NO_ERROR) {
		return error;
	}

	Lock lock;

	sRecords = {};
	sNextHandle = 1;

	AliroError firstError{ ALIRO_NO_ERROR };

	for (size_t i = 0; i < kMaxRecords; ++i) {
		Record loaded{};
		bool present{ false };
		error = Persistence::LoadRecord(i, loaded, present);
		if (error != ALIRO_NO_ERROR) {
			return error;
		}

		if (!present) {
			continue;
		}

		bool accept{ false };
		KeepFirstError(firstError, AcceptLoadedLocked(i, loaded, accept));
		if (!accept) {
			LOG_WRN("Dropping unrecoverable persistent-key record in slot %zu", i);
			KeepFirstError(firstError, Persistence::EraseRecord(i));
			continue;
		}

		sRecords[i] = loaded;
		if (loaded.mHandle >= sNextHandle) {
			sNextHandle = loaded.mHandle + 1;
		}
	}

	for (size_t i = 0; i < kMaxRecords; ++i) {
		KeepFirstError(firstError, SweepSlotLocked(i, sRecords[i].mValid ? sRecords[i].mPersistedKeyId : 0));
	}

	return firstError;
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

	/* Clear keys left by an earlier failed cleanup; the committed key, if any, is kept. */
	const AliroError sweepError = SweepSlotLocked(slotIndex, oldPersistedKeyId);
	if (sweepError != ALIRO_NO_ERROR) {
		return sweepError;
	}

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
		/* A failed destroy leaves an unreferenced key, swept later. */
		(void)Backend::Destroy(actualPersistedKeyId);
		return persistError;
	}

	*targetSlot = newRecord;
	if (existing == nullptr) {
		sNextHandle = newRecord.mHandle + 1;
	}

	/*
	 * Only now retire the old key: the new record is durably committed, so
	 * this call must report success. A failed retirement leaves an
	 * unreferenced key, swept later.
	 */
	if (oldPersistedKeyId != 0 && oldPersistedKeyId != actualPersistedKeyId) {
		(void)Backend::Destroy(oldPersistedKeyId);
	}

	outRecord = newRecord.mHandle;
	return ALIRO_NO_ERROR;
}

AliroError Delete(PersistentKeyHandle record)
{
	Lock lock;

	const Record *found = FindLocked(record);
	if (found == nullptr) {
		return ALIRO_NO_ERROR;
	}

	return RemoveSlotLocked(SlotIndexOf(found));
}

AliroError Reset()
{
	Lock lock;

	AliroError firstError{ ALIRO_NO_ERROR };

	for (size_t i = 0; i < kMaxRecords; ++i) {
		KeepFirstError(firstError, RemoveSlotLocked(i));
	}

	return firstError;
}

AliroError DeleteAllForCredential(CredentialHandle handle)
{
	Lock lock;

	AliroError firstError{ ALIRO_NO_ERROR };

	for (size_t i = 0; i < kMaxRecords; ++i) {
		if (sRecords[i].mValid && sRecords[i].mCredentialHandle == handle) {
			KeepFirstError(firstError, RemoveSlotLocked(i));
		}
	}

	return firstError;
}

} // namespace AliroUd::PersistentKey::Store
