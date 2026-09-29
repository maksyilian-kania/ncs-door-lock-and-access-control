/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include "credential_store.h"
#include "provisioning.h"

#include "storage/mailbox/mailbox_store.h"

#include <zephyr/logging/log.h>

#include <aliro/user_device/interface.h>
#include <aliro/utils.h>

/*
 * Thin adapter from Aliro::Interface::UserDevice::Credential/::Trust to
 * this application's own credential_store (APP_PLAN.md AWP3). Every
 * function here translates the wire `ConstData provisioningInput` to/from
 * `Provisioning::Payload` and calls into `AliroUd::Credential::Store`.
 * `Delete()`/`Reset()` also erase the deleted credentials' mailbox bytes,
 * which the public `Reset()` contract includes. No storage, trust, or
 * transaction logic lives in this file; see credential_store.cpp for that.
 */
LOG_MODULE_DECLARE(aliro_ud_credential, CONFIG_ALIRO_UD_CREDENTIAL_LOG_LEVEL);

namespace {

bool CredentialExists(::Aliro::UserDevice::CredentialHandle handle)
{
	::Aliro::UserDevice::CredentialMetadata metadata{};
	return AliroUd::Credential::Store::GetMetadata(handle, metadata) == ALIRO_NO_ERROR;
}

/* Mutations are serialized by AliroUd::Lifecycle::RunMutation(); a mailbox record is too large for its caller's stack. */
AliroUd::Mailbox::MailboxRecord sErasedMailbox{};

/*
 * The mailbox goes first so that no mailbox bytes outlive their credential
 * into a later credential at the same handle. If the credential deletion
 * then fails, the credential is still committed and gets its mailbox back.
 */
AliroError DeleteWithMailbox(::Aliro::UserDevice::CredentialHandle handle)
{
	auto error = AliroUd::Mailbox::Store::EraseForCredential(handle, sErasedMailbox);
	VerifyOrReturnStatus(error == ALIRO_NO_ERROR, error, LOG_ERR("Failed to erase mailbox of credential %u", handle));

	error = AliroUd::Credential::Store::Delete(handle);
	if (error != ALIRO_NO_ERROR && CredentialExists(handle) &&
	    AliroUd::Mailbox::Store::Restore(handle, sErasedMailbox) != ALIRO_NO_ERROR) {
		LOG_ERR("Failed to restore mailbox of credential %u", handle);
	}

	sErasedMailbox = AliroUd::Mailbox::MailboxRecord{};
	return error;
}

} // namespace

namespace Aliro::Interface::UserDevice::Credential {

AliroError Validate(ConstData provisioningInput)
{
	AliroUd::Credential::Provisioning::Payload payload{};

	if (!AliroUd::Credential::Provisioning::Parse(provisioningInput, payload)) {
		return ALIRO_INVALID_DATA_FORMAT;
	}

	return AliroUd::Credential::Store::Validate(payload);
}

AliroError Create(ConstData provisioningInput, ::Aliro::UserDevice::CredentialHandle &outHandle)
{
	outHandle = ::Aliro::UserDevice::kInvalidCredentialHandle;

	AliroUd::Credential::Provisioning::Payload payload{};
	if (!AliroUd::Credential::Provisioning::Parse(provisioningInput, payload)) {
		return ALIRO_INVALID_DATA_FORMAT;
	}

	return AliroUd::Credential::Store::Create(payload, outHandle);
}

AliroError Update(::Aliro::UserDevice::CredentialHandle handle, ConstData provisioningInput)
{
	AliroUd::Credential::Provisioning::Payload payload{};
	if (!AliroUd::Credential::Provisioning::Parse(provisioningInput, payload)) {
		return ALIRO_INVALID_DATA_FORMAT;
	}

	return AliroUd::Credential::Store::Update(handle, payload);
}

AliroError Delete(::Aliro::UserDevice::CredentialHandle handle)
{
	if (!CredentialExists(handle)) {
		return ALIRO_INVALID_ARGUMENT;
	}

	return DeleteWithMailbox(handle);
}

/*
 * Each credential is deleted on its own; one that fails keeps its
 * documents and mailbox, and the store-wide cleanup waits for a reset in
 * which every credential deletion succeeds.
 */
AliroError Reset()
{
	AliroError firstError{ ALIRO_NO_ERROR };

	constexpr auto kMaxCredentials =
		static_cast<::Aliro::UserDevice::CredentialHandle>(AliroUd::Credential::kMaxCredentials);
	for (::Aliro::UserDevice::CredentialHandle handle = 1; handle <= kMaxCredentials; ++handle) {
		if (!CredentialExists(handle)) {
			continue;
		}

		const auto error = DeleteWithMailbox(handle);
		if (error != ALIRO_NO_ERROR && firstError == ALIRO_NO_ERROR) {
			firstError = error;
		}
	}
	VerifyOrReturnStatus(firstError == ALIRO_NO_ERROR, firstError);

	const auto error = AliroUd::Credential::Store::Reset();
	VerifyOrReturnStatus(error == ALIRO_NO_ERROR, error);

	return AliroUd::Mailbox::Store::EraseAll();
}

AliroError GetGroupBindingCount(::Aliro::UserDevice::CredentialHandle handle, size_t &outCount)
{
	return AliroUd::Credential::Store::GetGroupBindingCount(handle, outCount);
}

AliroError GetGroupBinding(::Aliro::UserDevice::CredentialHandle handle, size_t index,
			   ::Aliro::UserDevice::ReaderGroupIdentifier &outReaderGroupIdentifier)
{
	return AliroUd::Credential::Store::GetGroupBinding(handle, index, outReaderGroupIdentifier);
}

AliroError ResolveByReaderGroupIdentifier(const ::Aliro::UserDevice::ReaderGroupIdentifier &readerGroupIdentifier,
					  ::Aliro::UserDevice::CredentialHandle *outHandles, size_t &inOutCount)
{
	return AliroUd::Credential::Store::ResolveByReaderGroupIdentifier(readerGroupIdentifier, outHandles,
									  inOutCount);
}

AliroError GetMetadata(::Aliro::UserDevice::CredentialHandle handle,
		       ::Aliro::UserDevice::CredentialMetadata &outMetadata)
{
	return AliroUd::Credential::Store::GetMetadata(handle, outMetadata);
}

AliroError GetSignedTimestamps(::Aliro::UserDevice::CredentialHandle handle,
			       ::Aliro::UserDevice::CredentialSignedTimestamps &outTimestamps)
{
	return AliroUd::Credential::Store::GetSignedTimestamps(handle, outTimestamps);
}

} // namespace Aliro::Interface::UserDevice::Credential

namespace Aliro::Interface::UserDevice::Trust {

AliroError GetReaderPublicKey(::Aliro::UserDevice::CredentialHandle handle,
			      const ::Aliro::UserDevice::ReaderGroupIdentifier &readerGroupIdentifier,
			      CryptoTypes::PublicKey &outPublicKey)
{
	return AliroUd::Credential::Store::GetReaderPublicKey(handle, readerGroupIdentifier, outPublicKey);
}

AliroError GetReaderIssuerPublicKey(::Aliro::UserDevice::CredentialHandle handle,
				    const ::Aliro::UserDevice::ReaderGroupIdentifier &readerGroupIdentifier,
				    CryptoTypes::PublicKey &outPublicKey)
{
	return AliroUd::Credential::Store::GetReaderIssuerPublicKey(handle, readerGroupIdentifier, outPublicKey);
}

} // namespace Aliro::Interface::UserDevice::Trust
