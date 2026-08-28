/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include "fake_key_backend.h"

#include <storage/credential/key_backend.h>

#include <psa/crypto.h>

#include <array>

using namespace Aliro;

namespace AliroUd::Credential::Test {
namespace {

struct Mapping {
	bool mInUse{ false };
	CryptoTypes::KeyId mDesiredId{ 0 };
	psa_key_id_t mVolatileId{ 0 };
};

/* Generous fixed capacity: one entry per possible slot key ID plus headroom for the scratch ID. */
std::array<Mapping, 64> sMappings{};
bool sPsaInitialized{ false };

Mapping *Find(CryptoTypes::KeyId desiredId)
{
	for (auto &mapping : sMappings) {
		if (mapping.mInUse && mapping.mDesiredId == desiredId) {
			return &mapping;
		}
	}
	return nullptr;
}

Mapping *FindFreeSlot()
{
	for (auto &mapping : sMappings) {
		if (!mapping.mInUse) {
			return &mapping;
		}
	}
	return nullptr;
}

} // namespace

void ResetFakeKeyBackend()
{
	for (auto &mapping : sMappings) {
		if (mapping.mInUse) {
			psa_destroy_key(mapping.mVolatileId);
		}
	}
	sMappings = {};
}

} // namespace AliroUd::Credential::Test

namespace AliroUd::Credential::KeyBackend {

AliroError Init()
{
	/*
	 * Idempotent and called once per simulated "reboot"
	 * (AliroUd::Credential::Store::Init()): must not disturb already-open
	 * volatile keys simulating persistence, so psa_crypto_init() itself
	 * only actually runs once per process.
	 */
	if (Test::sPsaInitialized) {
		return ALIRO_NO_ERROR;
	}

	const psa_status_t status = psa_crypto_init();
	if (status != PSA_SUCCESS) {
		return ALIRO_ERROR_INTERNAL;
	}

	Test::sPsaInitialized = true;
	return ALIRO_NO_ERROR;
}

AliroError ImportPrivateKeyScalar(CryptoTypes::KeyId desiredKeyId,
				  const std::array<uint8_t, CryptoTypes::kEccP256KeyPrivateKeyLength> &scalar,
				  CryptoTypes::PublicKey &outPublicKey)
{
	if (Test::Find(desiredKeyId) != nullptr) {
		return ALIRO_KEY_ALREADY_EXISTS;
	}

	psa_key_attributes_t attributes = PSA_KEY_ATTRIBUTES_INIT;
	psa_set_key_type(&attributes, PSA_KEY_TYPE_ECC_KEY_PAIR(PSA_ECC_FAMILY_SECP_R1));
	psa_set_key_algorithm(&attributes, PSA_ALG_ECDSA(PSA_ALG_SHA_256));
	psa_set_key_bits(&attributes, PSA_BYTES_TO_BITS(CryptoTypes::kEccP256KeyPrivateKeyLength));
	psa_set_key_usage_flags(&attributes, PSA_KEY_USAGE_SIGN_HASH);
	/* Deliberately left PSA_KEY_LIFETIME_VOLATILE (the default): no persistent/trusted
	 * storage backend exists on native_sim; see fake_key_backend.h. */

	psa_key_id_t volatileId{};
	const psa_status_t importStatus = psa_import_key(&attributes, scalar.data(), scalar.size(), &volatileId);
	if (importStatus != PSA_SUCCESS) {
		return ALIRO_INVALID_ARGUMENT;
	}

	size_t outLen{};
	const psa_status_t exportStatus = psa_export_public_key(volatileId, outPublicKey.data(), outPublicKey.size(),
								 &outLen);
	if (exportStatus != PSA_SUCCESS || outLen != outPublicKey.size()) {
		psa_destroy_key(volatileId);
		return ALIRO_ERROR_INTERNAL;
	}

	auto *slot = Test::FindFreeSlot();
	if (slot == nullptr) {
		psa_destroy_key(volatileId);
		return ALIRO_NO_MEMORY;
	}

	slot->mInUse = true;
	slot->mDesiredId = desiredKeyId;
	slot->mVolatileId = volatileId;

	return ALIRO_NO_ERROR;
}

AliroError DestroyKey(CryptoTypes::KeyId keyId)
{
	auto *mapping = Test::Find(keyId);
	if (mapping == nullptr) {
		return ALIRO_NO_ERROR;
	}

	psa_destroy_key(mapping->mVolatileId);
	*mapping = Test::Mapping{};

	return ALIRO_NO_ERROR;
}

bool IsKeyPresent(CryptoTypes::KeyId keyId)
{
	return Test::Find(keyId) != nullptr;
}

AliroError GetPublicKey(CryptoTypes::KeyId keyId, CryptoTypes::PublicKey &outPublicKey)
{
	auto *mapping = Test::Find(keyId);
	if (mapping == nullptr) {
		return ALIRO_ERROR_INTERNAL;
	}

	size_t outLen{};
	const psa_status_t status = psa_export_public_key(mapping->mVolatileId, outPublicKey.data(),
							   outPublicKey.size(), &outLen);
	if (status != PSA_SUCCESS || outLen != outPublicKey.size()) {
		return ALIRO_ERROR_INTERNAL;
	}

	return ALIRO_NO_ERROR;
}

} // namespace AliroUd::Credential::KeyBackend
