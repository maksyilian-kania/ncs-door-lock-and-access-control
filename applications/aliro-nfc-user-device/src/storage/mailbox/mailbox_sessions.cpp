/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include "mailbox_sessions.h"

#include "mailbox_store.h"

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <cstring>

LOG_MODULE_DECLARE(aliro_ud_mailbox, CONFIG_ALIRO_UD_MAILBOX_LOG_LEVEL);

using namespace Aliro;
using namespace Aliro::UserDevice;

namespace AliroUd::Mailbox::Sessions {
namespace {

struct Session {
	bool mActive{ false };
	SessionHandle mHandle{ kInvalidSessionHandle };
	CredentialHandle mCredentialHandle{ kInvalidCredentialHandle };
	std::array<uint8_t, kMaxSizeBytes> mStaged{};
	std::array<bool, kMaxSizeBytes> mDirty{};
};

K_MUTEX_DEFINE(sMutex);
std::array<Session, kMaxSessions> sSessions{};
SessionHandle sNextHandle{ 1 };

class Lock {
public:
	Lock() { k_mutex_lock(&sMutex, K_FOREVER); }
	~Lock() { k_mutex_unlock(&sMutex); }
	Lock(const Lock &) = delete;
	Lock &operator=(const Lock &) = delete;
};

/* Overflow-safe [offset, offset + length) <= size. */
bool InBounds(size_t offset, size_t length, size_t size)
{
	if (offset > size) {
		return false;
	}
	return length <= (size - offset);
}

Session *FindLocked(SessionHandle handle)
{
	if (handle == kInvalidSessionHandle) {
		return nullptr;
	}

	for (auto &session : sSessions) {
		if (session.mActive && session.mHandle == handle) {
			return &session;
		}
	}
	return nullptr;
}

} // namespace

AliroError OpenSnapshot(::Aliro::UserDevice::MailboxHandle handle, SessionHandle &outSession)
{
	outSession = kInvalidSessionHandle;
	Lock lock;

	const CredentialHandle credentialHandle = Store::CredentialForHandle(handle);

	Store::Config config{};
	const auto configError = Store::GetConfig(credentialHandle, config);
	if (configError != ALIRO_NO_ERROR) {
		return ALIRO_INVALID_ARGUMENT;
	}

	/*
	 * WP7 stack impact (see docs/wp7_stack_impact.md), amendment A6: at
	 * most one open snapshot per mailbox. Distinct ALIRO_INVALID_STATE
	 * from the ALIRO_INVALID_ARGUMENT above (invalid/absent mailbox).
	 */
	for (const auto &session : sSessions) {
		if (session.mActive && session.mCredentialHandle == credentialHandle) {
			return ALIRO_INVALID_STATE;
		}
	}

	Session *freeSlot{ nullptr };
	for (auto &session : sSessions) {
		if (!session.mActive) {
			freeSlot = &session;
			break;
		}
	}

	if (freeSlot == nullptr) {
		LOG_WRN("No free mailbox session slot (capacity %zu)", kMaxSessions);
		return ALIRO_NO_MEMORY;
	}

	/*
	 * Lazily ensure committed storage exists (Aliro 1.0 Specification,
	 * Appendix 18: absent content SHALL read as all-zero). Explicit CLI
	 * "init"/"reset" (APP_PLAN.md AWP6) remain idempotent no-ops once
	 * this has already run.
	 */
	const auto initError = Store::Initialize(credentialHandle);
	if (initError != ALIRO_NO_ERROR) {
		return initError;
	}

	*freeSlot = Session{};
	freeSlot->mActive = true;
	freeSlot->mHandle = sNextHandle++;
	if (sNextHandle == kInvalidSessionHandle) {
		sNextHandle = 1;
	}
	freeSlot->mCredentialHandle = credentialHandle;

	outSession = freeSlot->mHandle;
	return ALIRO_NO_ERROR;
}

AliroError Read(SessionHandle session, size_t offset, uint8_t *outData, size_t length)
{
	Lock lock;

	auto *found = FindLocked(session);
	if (found == nullptr) {
		return ALIRO_INVALID_STATE;
	}

	Store::Config config{};
	const auto configError = Store::GetConfig(found->mCredentialHandle, config);
	if (configError != ALIRO_NO_ERROR) {
		return ALIRO_INVALID_STATE;
	}

	if (!config.mPermissions.mReadable) {
		return ALIRO_INVALID_STATE;
	}

	if (!InBounds(offset, length, config.mSizeBytes)) {
		return ALIRO_INVALID_ARGUMENT;
	}

	/* Committed reads are isolated from this (or any other) session's staged writes (APP_PLAN.md AWP6). */
	return Store::RawRead(found->mCredentialHandle, offset, outData, length);
}

AliroError StageWrite(SessionHandle session, size_t offset, const uint8_t *data, size_t length)
{
	Lock lock;

	auto *found = FindLocked(session);
	if (found == nullptr) {
		return ALIRO_INVALID_STATE;
	}

	Store::Config config{};
	const auto configError = Store::GetConfig(found->mCredentialHandle, config);
	if (configError != ALIRO_NO_ERROR) {
		return ALIRO_INVALID_STATE;
	}

	if (!config.mPermissions.mWritable) {
		return ALIRO_INVALID_STATE;
	}

	if (!InBounds(offset, length, config.mSizeBytes)) {
		return ALIRO_INVALID_ARGUMENT;
	}

	for (size_t i = 0; i < length; ++i) {
		found->mStaged[offset + i] = data[i];
		found->mDirty[offset + i] = true;
	}

	return ALIRO_NO_ERROR;
}

AliroError StageSet(SessionHandle session, size_t offset, size_t length, uint8_t value)
{
	Lock lock;

	auto *found = FindLocked(session);
	if (found == nullptr) {
		return ALIRO_INVALID_STATE;
	}

	Store::Config config{};
	const auto configError = Store::GetConfig(found->mCredentialHandle, config);
	if (configError != ALIRO_NO_ERROR) {
		return ALIRO_INVALID_STATE;
	}

	if (!config.mPermissions.mWritable) {
		return ALIRO_INVALID_STATE;
	}

	/*
	 * WP7 stack impact (see docs/wp7_stack_impact.md), amendment A1:
	 * fill [offset, offset + length) with a single repeated byte (Table
	 * 8-16 0x95 set request), not a full-mailbox buffer as before.
	 */
	if (!InBounds(offset, length, config.mSizeBytes)) {
		return ALIRO_INVALID_ARGUMENT;
	}

	for (size_t i = 0; i < length; ++i) {
		found->mStaged[offset + i] = value;
		found->mDirty[offset + i] = true;
	}

	return ALIRO_NO_ERROR;
}

AliroError Commit(SessionHandle session)
{
	Lock lock;

	auto *found = FindLocked(session);
	if (found == nullptr) {
		return ALIRO_INVALID_STATE;
	}

	const auto error = Store::ApplyDirtyBytes(found->mCredentialHandle, found->mStaged, found->mDirty);
	if (error != ALIRO_NO_ERROR) {
		/* On failure, no staged changes are committed (interface.h contract); staged state is left intact
		 * so the caller may retry Commit() after resolving the underlying failure. */
		return error;
	}

	found->mStaged = {};
	found->mDirty = {};
	return ALIRO_NO_ERROR;
}

AliroError Rollback(SessionHandle session)
{
	Lock lock;

	auto *found = FindLocked(session);
	if (found == nullptr) {
		return ALIRO_INVALID_STATE;
	}

	found->mStaged = {};
	found->mDirty = {};
	return ALIRO_NO_ERROR;
}

void Close(SessionHandle session)
{
	Lock lock;

	auto *found = FindLocked(session);
	if (found == nullptr) {
		return;
	}

	*found = Session{};
}

size_t GetOpenSessionCount()
{
	Lock lock;

	size_t count = 0;
	for (const auto &session : sSessions) {
		if (session.mActive) {
			++count;
		}
	}
	return count;
}

} // namespace AliroUd::Mailbox::Sessions
