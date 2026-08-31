/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include "storage/credential/credential_store.h"
#include "storage/credential/key_backend.h"

#include <aliro/user_device/interface.h>

/*
 * Aliro::Interface::UserDevice::CredentialSigning contract (APP_PLAN.md
 * AWP5). Resolves the opaque CredentialHandle to its persistent PSA key
 * identifier via AliroUd::Credential::Store::GetFullRecord() (AWP3), then
 * signs only through AliroUd::Credential::KeyBackend::Sign() (the only
 * module that knows how an application-facing key ID maps to a usable PSA
 * key handle on the active backend, real or host-test fake) — the private
 * key scalar is never copied back into this module or any other.
 */
namespace Aliro::Interface::UserDevice::CredentialSigning {

AliroError Sign(::Aliro::UserDevice::CredentialHandle handle, const uint8_t *data, size_t dataLength,
		CryptoTypes::Signature &outSignature)
{
	outSignature.fill(0);

	if (data == nullptr || dataLength == 0) {
		return ALIRO_INVALID_ARGUMENT;
	}

	AliroUd::Credential::PersistedCredential record{};
	const AliroError lookupError = AliroUd::Credential::Store::GetFullRecord(handle, record);
	if (lookupError != ALIRO_NO_ERROR) {
		return lookupError;
	}

	return AliroUd::Credential::KeyBackend::Sign(record.mKeyId, data, dataLength, outSignature);
}

} // namespace Aliro::Interface::UserDevice::CredentialSigning
