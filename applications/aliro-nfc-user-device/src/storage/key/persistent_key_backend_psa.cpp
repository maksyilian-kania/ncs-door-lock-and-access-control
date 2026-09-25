/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include "persistent_key_backend.h"

#include <platform/crypto/crypto_internal.h>

#include <zephyr/logging/log.h>

#include <psa/crypto.h>

LOG_MODULE_DECLARE(aliro_ud_key, CONFIG_ALIRO_UD_KEY_LOG_LEVEL);

using namespace Aliro;

/*
 * Real PSA-backed implementation. Only built when
 * CONFIG_ALIRO_UD_PERSISTENT_KEY_BACKEND_REAL=y (disabled under ZTEST; see
 * Kconfig). Mirrors the explicit-persistent-ID pattern established by
 * storage/credential/key_backend_psa.cpp, but reaches the key material via
 * psa_copy_key() rather than psa_import_key(), since the source key here is
 * always an opaque, non-exportable `Aliro::Interface::UserDevice::Crypto`
 * marker handle (see platform/crypto/crypto_internal.h).
 */
namespace AliroUd::PersistentKey::Backend {
namespace {

psa_key_attributes_t GetPersistentKeyAttributes(::Aliro::CryptoTypes::KeyId keyId)
{
	psa_key_attributes_t attributes = PSA_KEY_ATTRIBUTES_INIT;

	psa_set_key_type(&attributes, PSA_KEY_TYPE_DERIVE);
	psa_set_key_algorithm(&attributes, PSA_ALG_HKDF(PSA_ALG_SHA_256));
	/* Kpersistent is only ever used via HKDF derivation; COPY is kept so Lookup() can copy back out. */
	psa_set_key_usage_flags(&attributes, PSA_KEY_USAGE_DERIVE | PSA_KEY_USAGE_COPY);
	psa_set_key_lifetime(&attributes, PSA_KEY_LIFETIME_PERSISTENT);
	psa_set_key_id(&attributes, keyId);

	return attributes;
}

psa_key_attributes_t GetVolatileKeyAttributes()
{
	psa_key_attributes_t attributes = PSA_KEY_ATTRIBUTES_INIT;

	psa_set_key_type(&attributes, PSA_KEY_TYPE_DERIVE);
	psa_set_key_algorithm(&attributes, PSA_ALG_HKDF(PSA_ALG_SHA_256));
	psa_set_key_usage_flags(&attributes, PSA_KEY_USAGE_DERIVE);
	psa_set_key_lifetime(&attributes, PSA_KEY_LIFETIME_VOLATILE);

	return attributes;
}

} // namespace

AliroError Exists(::Aliro::CryptoTypes::KeyId persistedKeyId, bool &outExists)
{
	outExists = false;
	if (persistedKeyId == 0) {
		return ALIRO_NO_ERROR;
	}

	psa_key_attributes_t attributes = PSA_KEY_ATTRIBUTES_INIT;
	const psa_status_t status = psa_get_key_attributes(static_cast<psa_key_id_t>(persistedKeyId), &attributes);
	psa_reset_key_attributes(&attributes);

	if (status == PSA_SUCCESS) {
		outExists = true;
		return ALIRO_NO_ERROR;
	}

	if (status == PSA_ERROR_INVALID_HANDLE || status == PSA_ERROR_DOES_NOT_EXIST) {
		return ALIRO_NO_ERROR;
	}

	LOG_ERR("psa_get_key_attributes(0x%08x) failed: %d", persistedKeyId, status);
	return ALIRO_ERROR_INTERNAL;
}

AliroError DurablyOwn(::Aliro::CryptoTypes::KeyId sourceKeyId, ::Aliro::CryptoTypes::KeyId desiredPersistentKeyId,
		      ::Aliro::CryptoTypes::KeyId &outActualPersistentKeyId)
{
	outActualPersistentKeyId = 0;

	const psa_key_id_t sourcePsaKeyId = AliroUd::Crypto::Internal::ResolveDeriveKeyIdForPersistence(sourceKeyId);
	if (sourcePsaKeyId == 0) {
		LOG_ERR("DurablyOwn(): source key 0x%08x does not resolve to a derive-capable key", sourceKeyId);
		return ALIRO_INVALID_ARGUMENT;
	}

	bool inUse{ false };
	const AliroError presenceError = Exists(desiredPersistentKeyId, inUse);
	if (presenceError != ALIRO_NO_ERROR) {
		return presenceError;
	}
	if (inUse) {
		LOG_ERR("DurablyOwn(): persistent key ID 0x%08x already in use", desiredPersistentKeyId);
		return ALIRO_KEY_ALREADY_EXISTS;
	}

	psa_key_attributes_t attributes = GetPersistentKeyAttributes(static_cast<psa_key_id_t>(desiredPersistentKeyId));
	psa_key_id_t copiedKeyId{};
	const psa_status_t status = psa_copy_key(sourcePsaKeyId, &attributes, &copiedKeyId);
	if (status != PSA_SUCCESS) {
		LOG_ERR("psa_copy_key(persist 0x%08x) failed: %d", desiredPersistentKeyId, status);
		return ALIRO_ERROR_INTERNAL;
	}

	outActualPersistentKeyId = desiredPersistentKeyId;
	return ALIRO_NO_ERROR;
}

AliroError MintVolatileHandle(::Aliro::CryptoTypes::KeyId persistedKeyId, ::Aliro::CryptoTypes::KeyId &outVolatileKeyId)
{
	outVolatileKeyId = 0;

	psa_key_attributes_t attributes = GetVolatileKeyAttributes();
	psa_key_id_t volatileKeyId{};
	const psa_status_t status =
		psa_copy_key(static_cast<psa_key_id_t>(persistedKeyId), &attributes, &volatileKeyId);
	if (status != PSA_SUCCESS) {
		LOG_ERR("psa_copy_key(volatile from 0x%08x) failed: %d", persistedKeyId, status);
		return ALIRO_ERROR_INTERNAL;
	}

	::Aliro::CryptoTypes::KeyId markerKeyId{};
	const AliroError registerError = AliroUd::Crypto::Internal::RegisterDeriveOnlyKey(volatileKeyId, markerKeyId);
	if (registerError != ALIRO_NO_ERROR) {
		psa_destroy_key(volatileKeyId);
		return registerError;
	}

	outVolatileKeyId = markerKeyId;
	return ALIRO_NO_ERROR;
}

AliroError Destroy(::Aliro::CryptoTypes::KeyId persistedKeyId)
{
	bool present{ false };
	const AliroError presenceError = Exists(persistedKeyId, present);
	if (presenceError != ALIRO_NO_ERROR || !present) {
		return presenceError;
	}

	const psa_status_t status = psa_destroy_key(static_cast<psa_key_id_t>(persistedKeyId));
	if (status != PSA_SUCCESS) {
		LOG_ERR("psa_destroy_key(0x%08x) failed: %d", persistedKeyId, status);
		return ALIRO_ERROR_INTERNAL;
	}

	return ALIRO_NO_ERROR;
}

} // namespace AliroUd::PersistentKey::Backend
