/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include "credential_store.h"
#include "provisioning.h"

#include <zephyr/logging/log.h>

#include <aliro/user_device/interface.h>

/*
 * Thin adapter from Aliro::Interface::UserDevice::Credential/::Trust to
 * this application's own credential_store (APP_PLAN.md AWP3). Every
 * function here does exactly two things: translate the wire
 * `ConstData provisioningInput` to/from `Provisioning::Payload`, and call
 * into `AliroUd::Credential::Store`. No storage, trust, or transaction
 * logic lives in this file; see credential_store.cpp for that.
 */
LOG_MODULE_DECLARE(aliro_ud_credential, CONFIG_ALIRO_UD_CREDENTIAL_LOG_LEVEL);

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
	return AliroUd::Credential::Store::Delete(handle);
}

AliroError Reset()
{
	return AliroUd::Credential::Store::Reset();
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
