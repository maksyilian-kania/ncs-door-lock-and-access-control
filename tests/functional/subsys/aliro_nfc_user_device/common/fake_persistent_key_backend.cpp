/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include "fake_persistent_key_backend.h"

#include <platform/crypto/crypto_internal.h>
#include <storage/key/persistent_key_backend.h>
#include <storage/key/persistent_key_types.h>

#include <psa/crypto.h>

#include <array>

using namespace Aliro;

namespace AliroUd::PersistentKey::Test {
namespace {

struct Mapping {
	bool mInUse{ false };
	CryptoTypes::KeyId mPersistedId{ 0 };
	psa_key_id_t mVolatileId{ 0 };
};

/* One committed plus one staged key ID per record slot (see persistent_key_store.cpp). */
std::array<Mapping, 2 * kMaxRecords> sMappings{};

Mapping *Find(CryptoTypes::KeyId persistedId)
{
	for (auto &mapping : sMappings) {
		if (mapping.mInUse && mapping.mPersistedId == persistedId) {
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

psa_key_attributes_t GetKeyAttributes(psa_key_usage_t usage)
{
	psa_key_attributes_t attributes = PSA_KEY_ATTRIBUTES_INIT;

	psa_set_key_type(&attributes, PSA_KEY_TYPE_DERIVE);
	psa_set_key_algorithm(&attributes, PSA_ALG_HKDF(PSA_ALG_SHA_256));
	psa_set_key_usage_flags(&attributes, usage);

	return attributes;
}

} // namespace

void ResetFakePersistentKeyBackend()
{
	for (auto &mapping : sMappings) {
		if (mapping.mInUse) {
			psa_destroy_key(mapping.mVolatileId);
		}
	}
	sMappings = {};
}

} // namespace AliroUd::PersistentKey::Test

namespace AliroUd::PersistentKey::Backend {

AliroError DurablyOwn(CryptoTypes::KeyId sourceKeyId, CryptoTypes::KeyId desiredPersistentKeyId,
		      CryptoTypes::KeyId &outActualPersistentKeyId)
{
	outActualPersistentKeyId = 0;

	const psa_key_id_t sourcePsaKeyId = AliroUd::Crypto::Internal::ResolveDeriveKeyIdForPersistence(sourceKeyId);
	if (sourcePsaKeyId == 0) {
		return ALIRO_INVALID_ARGUMENT;
	}

	if (Test::Find(desiredPersistentKeyId) != nullptr) {
		return ALIRO_KEY_ALREADY_EXISTS;
	}

	auto *slot = Test::FindFreeSlot();
	if (slot == nullptr) {
		return ALIRO_NO_MEMORY;
	}

	psa_key_attributes_t attributes = Test::GetKeyAttributes(PSA_KEY_USAGE_DERIVE | PSA_KEY_USAGE_COPY);
	psa_key_id_t copiedKeyId{};
	if (psa_copy_key(sourcePsaKeyId, &attributes, &copiedKeyId) != PSA_SUCCESS) {
		return ALIRO_ERROR_INTERNAL;
	}

	slot->mInUse = true;
	slot->mPersistedId = desiredPersistentKeyId;
	slot->mVolatileId = copiedKeyId;

	outActualPersistentKeyId = desiredPersistentKeyId;
	return ALIRO_NO_ERROR;
}

AliroError MintVolatileHandle(CryptoTypes::KeyId persistedKeyId, CryptoTypes::KeyId &outVolatileKeyId)
{
	outVolatileKeyId = 0;

	auto *mapping = Test::Find(persistedKeyId);
	if (mapping == nullptr) {
		return ALIRO_ERROR_INTERNAL;
	}

	psa_key_attributes_t attributes = Test::GetKeyAttributes(PSA_KEY_USAGE_DERIVE);
	psa_key_id_t volatileKeyId{};
	if (psa_copy_key(mapping->mVolatileId, &attributes, &volatileKeyId) != PSA_SUCCESS) {
		return ALIRO_ERROR_INTERNAL;
	}

	CryptoTypes::KeyId markerKeyId{};
	const AliroError registerError = AliroUd::Crypto::Internal::RegisterDeriveOnlyKey(volatileKeyId, markerKeyId);
	if (registerError != ALIRO_NO_ERROR) {
		psa_destroy_key(volatileKeyId);
		return registerError;
	}

	outVolatileKeyId = markerKeyId;
	return ALIRO_NO_ERROR;
}

AliroError Destroy(CryptoTypes::KeyId persistedKeyId)
{
	auto *mapping = Test::Find(persistedKeyId);
	if (mapping == nullptr) {
		return ALIRO_NO_ERROR;
	}

	psa_destroy_key(mapping->mVolatileId);
	*mapping = Test::Mapping{};

	return ALIRO_NO_ERROR;
}

} // namespace AliroUd::PersistentKey::Backend
