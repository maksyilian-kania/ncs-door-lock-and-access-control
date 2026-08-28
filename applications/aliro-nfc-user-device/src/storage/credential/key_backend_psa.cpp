/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include "key_backend.h"

#include <zephyr/logging/log.h>

#include <psa/crypto.h>

LOG_MODULE_DECLARE(aliro_ud_credential, CONFIG_ALIRO_UD_CREDENTIAL_LOG_LEVEL);

/*
 * Real PSA/CRACEN/KMU-backed implementation. Only built when
 * CONFIG_ALIRO_UD_CREDENTIAL_KEY_BACKEND_REAL=y (disabled under ZTEST; see
 * Kconfig). Mirrors the persistent-key pattern established by
 * subsys/aliro/crypto_utils/src/crypto_utils.cpp, restricted to what AWP3
 * needs: import an externally-supplied P-256 private-key scalar at an
 * explicit, caller-chosen persistent key ID, sign-only (never exportable),
 * and derive its public key.
 */
namespace AliroUd::Credential::KeyBackend {
namespace {

psa_key_attributes_t GetPrivateKeyAttributes(::Aliro::CryptoTypes::KeyId keyId)
{
	psa_key_attributes_t attributes = PSA_KEY_ATTRIBUTES_INIT;

	psa_set_key_type(&attributes, PSA_KEY_TYPE_ECC_KEY_PAIR(PSA_ECC_FAMILY_SECP_R1));
	psa_set_key_algorithm(&attributes, PSA_ALG_ECDSA(PSA_ALG_SHA_256));
	psa_set_key_bits(&attributes, PSA_BYTES_TO_BITS(::Aliro::CryptoTypes::kEccP256KeyPrivateKeyLength));
	/* Sign-only: the private key is never exportable once imported (ALIRO-UD-SYRS-P1-008). */
	psa_set_key_usage_flags(&attributes, PSA_KEY_USAGE_SIGN_HASH);
	psa_set_key_lifetime(&attributes, PSA_KEY_LIFETIME_PERSISTENT);
	psa_set_key_id(&attributes, keyId);

	return attributes;
}

} // namespace

AliroError Init()
{
	const psa_status_t status = psa_crypto_init();

	if (status != PSA_SUCCESS && status != PSA_ERROR_ALREADY_EXISTS) {
		LOG_ERR("psa_crypto_init() failed: %d", status);
		return ALIRO_ERROR_INTERNAL;
	}

	return ALIRO_NO_ERROR;
}

AliroError ImportPrivateKeyScalar(::Aliro::CryptoTypes::KeyId desiredKeyId,
				  const std::array<uint8_t, ::Aliro::CryptoTypes::kEccP256KeyPrivateKeyLength> &scalar,
				  ::Aliro::CryptoTypes::PublicKey &outPublicKey)
{
	if (IsKeyPresent(desiredKeyId)) {
		LOG_ERR("Key ID 0x%08x already in use", desiredKeyId);
		return ALIRO_KEY_ALREADY_EXISTS;
	}

	psa_key_attributes_t attributes = GetPrivateKeyAttributes(desiredKeyId);
	psa_key_id_t importedId{};
	const psa_status_t importStatus = psa_import_key(&attributes, scalar.data(), scalar.size(), &importedId);

	if (importStatus != PSA_SUCCESS) {
		LOG_WRN("psa_import_key() rejected private-key scalar: %d", importStatus);
		return ALIRO_INVALID_ARGUMENT;
	}

	size_t outLen{};
	const psa_status_t exportStatus =
		psa_export_public_key(importedId, outPublicKey.data(), outPublicKey.size(), &outLen);

	if (exportStatus != PSA_SUCCESS || outLen != outPublicKey.size()) {
		LOG_ERR("psa_export_public_key() failed after import: %d", exportStatus);
		psa_destroy_key(importedId);
		return ALIRO_ERROR_INTERNAL;
	}

	return ALIRO_NO_ERROR;
}

AliroError DestroyKey(::Aliro::CryptoTypes::KeyId keyId)
{
	if (!IsKeyPresent(keyId)) {
		return ALIRO_NO_ERROR;
	}

	const psa_status_t status = psa_destroy_key(keyId);

	if (status != PSA_SUCCESS) {
		LOG_ERR("psa_destroy_key(0x%08x) failed: %d", keyId, status);
		return ALIRO_ERROR_INTERNAL;
	}

	return ALIRO_NO_ERROR;
}

bool IsKeyPresent(::Aliro::CryptoTypes::KeyId keyId)
{
	if (keyId == 0) {
		return false;
	}

	psa_key_attributes_t attributes = PSA_KEY_ATTRIBUTES_INIT;
	const psa_status_t status = psa_get_key_attributes(keyId, &attributes);
	psa_reset_key_attributes(&attributes);

	return status == PSA_SUCCESS;
}

AliroError GetPublicKey(::Aliro::CryptoTypes::KeyId keyId, ::Aliro::CryptoTypes::PublicKey &outPublicKey)
{
	size_t outLen{};
	const psa_status_t status = psa_export_public_key(keyId, outPublicKey.data(), outPublicKey.size(), &outLen);

	if (status != PSA_SUCCESS || outLen != outPublicKey.size()) {
		LOG_ERR("psa_export_public_key(0x%08x) failed: %d", keyId, status);
		return ALIRO_ERROR_INTERNAL;
	}

	return ALIRO_NO_ERROR;
}

} // namespace AliroUd::Credential::KeyBackend
