/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include <zephyr/logging/log.h>

#include <aliro/user_device/interface.h>

/*
 * Placeholder implementation of Aliro::Interface::UserDevice::Credential and
 * ::Trust, added only so the application links against the currently
 * checked-out stack (ncs-aliro WP5 introduces CredentialManager, which calls
 * into these contracts unconditionally). No credential/trust persistence
 * exists yet: every credential-set behaves as empty and every trust lookup
 * behaves as "not provisioned". Real Zephyr settings/NVS-backed storage,
 * per-binding trust, and the persistent transaction journal are implemented
 * in AWP3 (APP_PLAN.md); do not add real persistence here.
 */
LOG_MODULE_REGISTER(aliro_ud_credential, LOG_LEVEL_WRN);

namespace Aliro::Interface::UserDevice::Credential {

AliroError Validate(ConstData provisioningInput)
{
	ARG_UNUSED(provisioningInput);
	LOG_WRN("Credential::Validate() stub: no credential backend yet (AWP3)");
	return ALIRO_ERROR_NOT_IMPLEMENTED;
}

AliroError Create(ConstData provisioningInput, ::Aliro::UserDevice::CredentialHandle &outHandle)
{
	ARG_UNUSED(provisioningInput);
	outHandle = ::Aliro::UserDevice::kInvalidCredentialHandle;
	LOG_WRN("Credential::Create() stub: no credential backend yet (AWP3)");
	return ALIRO_ERROR_NOT_IMPLEMENTED;
}

AliroError Update(::Aliro::UserDevice::CredentialHandle handle, ConstData provisioningInput)
{
	ARG_UNUSED(handle);
	ARG_UNUSED(provisioningInput);
	LOG_WRN("Credential::Update() stub: no credential backend yet (AWP3)");
	return ALIRO_ERROR_NOT_IMPLEMENTED;
}

AliroError Delete(::Aliro::UserDevice::CredentialHandle handle)
{
	ARG_UNUSED(handle);
	LOG_WRN("Credential::Delete() stub: no credential backend yet (AWP3)");
	return ALIRO_ERROR_NOT_IMPLEMENTED;
}

AliroError Reset()
{
	LOG_WRN("Credential::Reset() stub: no credential backend yet (AWP3)");
	return ALIRO_ERROR_NOT_IMPLEMENTED;
}

size_t GetGroupBindingCount(::Aliro::UserDevice::CredentialHandle handle)
{
	ARG_UNUSED(handle);
	return 0;
}

AliroError GetGroupBinding(::Aliro::UserDevice::CredentialHandle handle, size_t index,
			   ReaderGroupIdentifier &outReaderGroupIdentifier)
{
	ARG_UNUSED(handle);
	ARG_UNUSED(index);
	ARG_UNUSED(outReaderGroupIdentifier);
	return ALIRO_ERROR_NOT_IMPLEMENTED;
}

AliroError ResolveByReaderGroupIdentifier(const ReaderGroupIdentifier &readerGroupIdentifier,
					  ::Aliro::UserDevice::CredentialHandle *outHandles, size_t &inOutCount)
{
	/*
	 * No credential is provisioned yet: zero matches is a well-defined,
	 * successful outcome per this contract's own documentation ("ALIRO_NO_ERROR
	 * on success (including zero matches)"), and lets
	 * CredentialManager::EvaluateAuth0() resolve to Auth0Outcome::kNoMatch
	 * cleanly rather than treating an unimplemented backend as a failure.
	 */
	ARG_UNUSED(readerGroupIdentifier);
	ARG_UNUSED(outHandles);
	inOutCount = 0;
	return ALIRO_NO_ERROR;
}

AliroError GetMetadata(::Aliro::UserDevice::CredentialHandle handle,
		       ::Aliro::UserDevice::CredentialMetadata &outMetadata)
{
	/*
	 * Unreachable in practice today: ResolveByReaderGroupIdentifier()
	 * above never yields a valid handle, so no caller currently reaches
	 * this with anything but an invalid handle. Kept as a real stub
	 * (rather than omitted) so any future caller fails loudly and
	 * deterministically instead of linking against a missing symbol.
	 */
	ARG_UNUSED(handle);
	outMetadata = ::Aliro::UserDevice::CredentialMetadata{};
	return ALIRO_ERROR_NOT_IMPLEMENTED;
}

} // namespace Aliro::Interface::UserDevice::Credential

namespace Aliro::Interface::UserDevice::Trust {

AliroError GetReaderPublicKey(::Aliro::UserDevice::CredentialHandle handle, CryptoTypes::PublicKey &outPublicKey)
{
	ARG_UNUSED(handle);
	outPublicKey = CryptoTypes::PublicKey{};
	return ALIRO_PUBLIC_KEY_NOT_FOUND;
}

AliroError GetReaderIssuerPublicKey(::Aliro::UserDevice::CredentialHandle handle, CryptoTypes::PublicKey &outPublicKey)
{
	ARG_UNUSED(handle);
	outPublicKey = CryptoTypes::PublicKey{};
	return ALIRO_PUBLIC_KEY_NOT_FOUND;
}

} // namespace Aliro::Interface::UserDevice::Trust
