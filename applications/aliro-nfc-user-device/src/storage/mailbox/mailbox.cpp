/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include "mailbox_sessions.h"
#include "mailbox_store.h"

#include <aliro/user_device/interface.h>

/*
 * Thin adapter from Aliro::Interface::UserDevice::Mailbox to this
 * application's own mailbox_sessions/mailbox_store engines (APP_PLAN.md
 * AWP6). Every function here does exactly one thing: forward to
 * AliroUd::Mailbox::Sessions or AliroUd::Mailbox::Store. No storage,
 * bounds, or permission logic lives in this file; see
 * mailbox_sessions.cpp/mailbox_store.cpp for that.
 *
 * WP7 stack impact (see docs/wp7_stack_impact.md): ResolveForCredential(),
 * GetMetadata(), HasNonZeroData(), GetMailboxDataSubsetPairCount(), and
 * GetMailboxDataSubsetPair() are new contract functions added upstream in
 * WP7-S2/WP7-S8 and are implemented here for the first time. StageSet()'s
 * signature changed (amendment A1, breaking).
 */
namespace Aliro::Interface::UserDevice::Mailbox {

AliroError ResolveForCredential(::Aliro::UserDevice::CredentialHandle handle,
				 ::Aliro::UserDevice::MailboxHandle &outHandle)
{
	outHandle = ::Aliro::UserDevice::kInvalidMailboxHandle;

	AliroUd::Mailbox::Store::Config config{};
	const auto handleForCredential = AliroUd::Mailbox::Store::HandleForCredential(handle);
	const auto error = AliroUd::Mailbox::Store::GetConfig(handle, config);
	if (error != ALIRO_NO_ERROR || !config.mConfigured) {
		/* A credential with no provisioned mailbox and an invalid CredentialHandle are observably
		 * identical (interface.h contract; ALIRO-UD-SYRS-P1-024/P1-031). */
		return ALIRO_INVALID_STATE;
	}

	outHandle = handleForCredential;
	return ALIRO_NO_ERROR;
}

AliroError GetMetadata(::Aliro::UserDevice::MailboxHandle handle, ::Aliro::UserDevice::MailboxMetadata &outMetadata)
{
	outMetadata = ::Aliro::UserDevice::MailboxMetadata{};

	AliroUd::Mailbox::Store::Config config{};
	const auto error = AliroUd::Mailbox::Store::GetConfig(AliroUd::Mailbox::Store::CredentialForHandle(handle), config);
	if (error != ALIRO_NO_ERROR) {
		return ALIRO_INVALID_ARGUMENT;
	}

	outMetadata.mHandle = handle;
	outMetadata.mSizeBytes = config.mSizeBytes;
	outMetadata.mPermissions = config.mPermissions;
	return ALIRO_NO_ERROR;
}

AliroError HasNonZeroData(::Aliro::UserDevice::MailboxHandle handle, bool &outHasNonZeroData)
{
	outHasNonZeroData = false;

	const auto credentialHandle = AliroUd::Mailbox::Store::CredentialForHandle(handle);
	AliroUd::Mailbox::Store::Config config{};
	if (AliroUd::Mailbox::Store::GetConfig(credentialHandle, config) != ALIRO_NO_ERROR) {
		return ALIRO_INVALID_ARGUMENT;
	}

	/* Committed data only (WP7-S1 decision D6): no atomic session exists before AUTH1. */
	outHasNonZeroData = AliroUd::Mailbox::Store::HasNonZeroData(credentialHandle);
	return ALIRO_NO_ERROR;
}

AliroError GetMailboxDataSubsetPairCount(::Aliro::UserDevice::MailboxHandle handle, bool &outConfigured,
					  size_t &outCount)
{
	outConfigured = false;
	outCount = 0;

	AliroUd::Mailbox::Store::Config config{};
	const auto error = AliroUd::Mailbox::Store::GetConfig(AliroUd::Mailbox::Store::CredentialForHandle(handle), config);
	if (error != ALIRO_NO_ERROR) {
		return ALIRO_INVALID_ARGUMENT;
	}

	outConfigured = config.mDataSubsetConfigured;
	outCount = config.mDataSubsetPairCount;
	return ALIRO_NO_ERROR;
}

AliroError GetMailboxDataSubsetPair(::Aliro::UserDevice::MailboxHandle handle, size_t index, uint16_t &outOffset,
				     uint16_t &outLength)
{
	outOffset = 0;
	outLength = 0;

	AliroUd::Mailbox::Store::Config config{};
	const auto error = AliroUd::Mailbox::Store::GetConfig(AliroUd::Mailbox::Store::CredentialForHandle(handle), config);
	if (error != ALIRO_NO_ERROR) {
		return ALIRO_INVALID_ARGUMENT;
	}

	if (index >= config.mDataSubsetPairCount) {
		return ALIRO_INVALID_ARGUMENT;
	}

	outOffset = config.mDataSubsetPairs[index].mOffset;
	outLength = config.mDataSubsetPairs[index].mLength;
	return ALIRO_NO_ERROR;
}

AliroError OpenSnapshot(::Aliro::UserDevice::MailboxHandle handle, SessionHandle &outSession)
{
	return AliroUd::Mailbox::Sessions::OpenSnapshot(handle, outSession);
}

AliroError Read(SessionHandle session, size_t offset, uint8_t *outData, size_t length)
{
	return AliroUd::Mailbox::Sessions::Read(session, offset, outData, length);
}

AliroError StageWrite(SessionHandle session, size_t offset, const uint8_t *data, size_t length)
{
	return AliroUd::Mailbox::Sessions::StageWrite(session, offset, data, length);
}

AliroError StageSet(SessionHandle session, size_t offset, size_t length, uint8_t value)
{
	return AliroUd::Mailbox::Sessions::StageSet(session, offset, length, value);
}

AliroError Commit(SessionHandle session)
{
	return AliroUd::Mailbox::Sessions::Commit(session);
}

AliroError Rollback(SessionHandle session)
{
	return AliroUd::Mailbox::Sessions::Rollback(session);
}

void Close(SessionHandle session)
{
	AliroUd::Mailbox::Sessions::Close(session);
}

} // namespace Aliro::Interface::UserDevice::Mailbox
