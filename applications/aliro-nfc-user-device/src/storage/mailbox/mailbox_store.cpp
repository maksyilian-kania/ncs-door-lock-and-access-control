/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include "mailbox_store.h"

#include "mailbox_persistence.h"
#include "storage/credential/credential_store.h"

#include <aliro/utils.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <cstring>

LOG_MODULE_REGISTER(aliro_ud_mailbox, CONFIG_ALIRO_UD_MAILBOX_LOG_LEVEL);

using namespace Aliro;
using namespace Aliro::UserDevice;

namespace AliroUd::Mailbox::Store {
namespace {

K_MUTEX_DEFINE(sMutex);

std::array<MailboxRecord, kMaxCredentials> sSlots{};

class Lock {
public:
	Lock() { k_mutex_lock(&sMutex, K_FOREVER); }
	~Lock() { k_mutex_unlock(&sMutex); }
	Lock(const Lock &) = delete;
	Lock &operator=(const Lock &) = delete;
};

bool HandleToSlotIndex(CredentialHandle handle, size_t &outIndex)
{
	if (handle == kInvalidCredentialHandle || handle > kMaxCredentials) {
		return false;
	}

	outIndex = static_cast<size_t>(handle - 1);
	return true;
}

/* Bounds-checked, overflow-safe [offset, offset + length) <= size. */
bool InBounds(size_t offset, size_t length, size_t size)
{
	if (offset > size) {
		return false;
	}
	return length <= (size - offset);
}

/* Reads the credential's mailbox configuration without taking sMutex (caller already holds it or doesn't need to). */
AliroError GetConfigLocked(CredentialHandle handle, Config &outConfig)
{
	outConfig = Config{};

	AliroUd::Credential::PersistedCredential record{};
	const auto error = AliroUd::Credential::Store::GetFullRecord(handle, record);
	if (error != ALIRO_NO_ERROR) {
		return ALIRO_INVALID_ARGUMENT;
	}

	if (!record.mMailbox.mConfigured) {
		return ALIRO_INVALID_ARGUMENT;
	}

	outConfig.mConfigured = true;
	outConfig.mSizeBytes = record.mMailbox.mSizeBytes;
	outConfig.mPermissions.mReadable = record.mMailbox.mReadable;
	outConfig.mPermissions.mWritable = record.mMailbox.mWritable;
	/* WP7 stack impact (see docs/wp7_stack_impact.md): mSettableInAuth1 no longer exists; the
	 * AUTH1 mailbox_data_subset descriptor is copied separately below instead. */
	outConfig.mDataSubsetConfigured = record.mMailbox.mDataSubsetConfigured;
	outConfig.mDataSubsetPairCount = record.mMailbox.mDataSubsetPairCount;
	outConfig.mDataSubsetPairs = record.mMailbox.mDataSubsetPairs;
	return ALIRO_NO_ERROR;
}

} // namespace

AliroError Init()
{
	Lock lock;

	auto error = Persistence::Init();
	VerifyOrReturnStatus(error == ALIRO_NO_ERROR, error, LOG_ERR("Mailbox persistence init failed"));

	for (size_t i = 0; i < kMaxCredentials; ++i) {
		bool present{ false };
		error = Persistence::LoadSlot(i, sSlots[i], present);
		if (error != ALIRO_NO_ERROR) {
			LOG_ERR("Failed to load mailbox slot %zu", i);
			sSlots[i] = MailboxRecord{};
		} else if (!present) {
			sSlots[i] = MailboxRecord{};
		}
	}

	return ALIRO_NO_ERROR;
}

AliroError GetConfig(CredentialHandle credentialHandle, Config &outConfig)
{
	Lock lock;
	return GetConfigLocked(credentialHandle, outConfig);
}

AliroError Initialize(CredentialHandle credentialHandle)
{
	Lock lock;

	Config config{};
	auto error = GetConfigLocked(credentialHandle, config);
	VerifyOrReturnStatus(error == ALIRO_NO_ERROR, error);

	size_t slotIndex{};
	if (!HandleToSlotIndex(credentialHandle, slotIndex)) {
		return ALIRO_INVALID_ARGUMENT;
	}

	if (sSlots[slotIndex].mInitialized) {
		/* Idempotent: never silently wipe already-committed mailbox content. */
		return ALIRO_NO_ERROR;
	}

	MailboxRecord record{};
	record.mInitialized = true;

	error = Persistence::SaveSlot(slotIndex, record);
	VerifyOrReturnStatus(error == ALIRO_NO_ERROR, error, LOG_ERR("Failed to persist mailbox slot %zu", slotIndex));

	sSlots[slotIndex] = record;
	return ALIRO_NO_ERROR;
}

AliroError Reset(CredentialHandle credentialHandle)
{
	Lock lock;

	Config config{};
	auto error = GetConfigLocked(credentialHandle, config);
	VerifyOrReturnStatus(error == ALIRO_NO_ERROR, error);

	size_t slotIndex{};
	if (!HandleToSlotIndex(credentialHandle, slotIndex)) {
		return ALIRO_INVALID_ARGUMENT;
	}

	MailboxRecord record{};
	record.mInitialized = true;

	error = Persistence::SaveSlot(slotIndex, record);
	VerifyOrReturnStatus(error == ALIRO_NO_ERROR, error, LOG_ERR("Failed to persist mailbox slot %zu", slotIndex));

	sSlots[slotIndex] = record;
	return ALIRO_NO_ERROR;
}

bool IsInitialized(CredentialHandle credentialHandle)
{
	Lock lock;

	size_t slotIndex{};
	if (!HandleToSlotIndex(credentialHandle, slotIndex)) {
		return false;
	}

	return sSlots[slotIndex].mInitialized;
}

bool HasNonZeroData(CredentialHandle credentialHandle)
{
	Lock lock;

	size_t slotIndex{};
	if (!HandleToSlotIndex(credentialHandle, slotIndex) || !sSlots[slotIndex].mInitialized) {
		return false;
	}

	Config config{};
	if (GetConfigLocked(credentialHandle, config) != ALIRO_NO_ERROR) {
		return false;
	}

	const auto &data = sSlots[slotIndex].mData;
	for (size_t i = 0; i < config.mSizeBytes; ++i) {
		if (data[i] != 0) {
			return true;
		}
	}
	return false;
}

AliroError RawRead(CredentialHandle credentialHandle, size_t offset, uint8_t *outData, size_t length)
{
	Lock lock;

	Config config{};
	auto error = GetConfigLocked(credentialHandle, config);
	VerifyOrReturnStatus(error == ALIRO_NO_ERROR, error);

	if (!InBounds(offset, length, config.mSizeBytes)) {
		return ALIRO_INVALID_ARGUMENT;
	}

	size_t slotIndex{};
	if (!HandleToSlotIndex(credentialHandle, slotIndex) || !sSlots[slotIndex].mInitialized) {
		return ALIRO_INVALID_STATE;
	}

	if (length > 0) {
		std::memcpy(outData, &sSlots[slotIndex].mData[offset], length);
	}
	return ALIRO_NO_ERROR;
}

AliroError ApplyDirtyBytes(CredentialHandle credentialHandle, const std::array<uint8_t, kMaxSizeBytes> &data,
			   const std::array<bool, kMaxSizeBytes> &dirty)
{
	Lock lock;

	Config config{};
	auto error = GetConfigLocked(credentialHandle, config);
	VerifyOrReturnStatus(error == ALIRO_NO_ERROR, error);

	size_t slotIndex{};
	if (!HandleToSlotIndex(credentialHandle, slotIndex) || !sSlots[slotIndex].mInitialized) {
		return ALIRO_INVALID_STATE;
	}

	/* Apply to a scratch copy first: a persistence failure must leave the committed record unchanged. */
	MailboxRecord updated = sSlots[slotIndex];
	for (size_t i = 0; i < config.mSizeBytes; ++i) {
		if (dirty[i]) {
			updated.mData[i] = data[i];
		}
	}

	error = Persistence::SaveSlot(slotIndex, updated);
	VerifyOrReturnStatus(error == ALIRO_NO_ERROR, error,
			     LOG_ERR("Failed to persist mailbox commit for slot %zu", slotIndex));

	sSlots[slotIndex] = updated;
	return ALIRO_NO_ERROR;
}

AliroError EraseForCredential(CredentialHandle credentialHandle)
{
	Lock lock;

	size_t slotIndex{};
	if (!HandleToSlotIndex(credentialHandle, slotIndex)) {
		return ALIRO_INVALID_ARGUMENT;
	}

	if (!sSlots[slotIndex].mInitialized) {
		return ALIRO_NO_ERROR;
	}

	const auto error = Persistence::EraseSlot(slotIndex);
	VerifyOrReturnStatus(error == ALIRO_NO_ERROR, error, LOG_ERR("Failed to erase mailbox slot %zu", slotIndex));

	sSlots[slotIndex] = MailboxRecord{};
	return ALIRO_NO_ERROR;
}

AliroError EraseAll()
{
	AliroError firstError{ ALIRO_NO_ERROR };

	for (size_t i = 0; i < kMaxCredentials; ++i) {
		const auto error = EraseForCredential(static_cast<CredentialHandle>(i + 1));
		if (error != ALIRO_NO_ERROR && firstError == ALIRO_NO_ERROR) {
			firstError = error;
		}
	}

	return firstError;
}

} // namespace AliroUd::Mailbox::Store
