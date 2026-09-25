/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include "fake_psa_key_backend.h"
#include "fake_power_loss.h"

#include <storage/key/persistent_key_backend.h>
#include <storage/key/persistent_key_types.h>

#include <algorithm>

using namespace Aliro;

namespace AliroUd::PersistentKey::FakePsa {
namespace {

/* Caller-owned IDs never overlap CONFIG_ALIRO_UD_PERSISTENT_KEY_ID_BASE's range and are never reused. */
constexpr CryptoTypes::KeyId kInputKeyIdBase{ 0x40000000 };
constexpr CryptoTypes::KeyId kTemporaryKeyIdBase{ 0x50000000 };

struct Object {
	bool mLive{ false };
	Kind mKind{ Kind::Input };
	CryptoTypes::KeyId mId{ 0 };
	Material mMaterial{};
};

/* Every durable slot ID plus generous room for caller-owned objects. */
std::array<Object, 2 * kMaxRecords + 64> sObjects{};
CryptoTypes::KeyId sNextInputKeyId{ kInputKeyIdBase + 1 };
CryptoTypes::KeyId sNextTemporaryKeyId{ kTemporaryKeyIdBase + 1 };
Fault sArmedFault{ Fault::None };
size_t sArmedSkip{ 0 };
size_t sDurableDestroyCount{ 0 };
size_t sDurableCreateCount{ 0 };
size_t sInvalidDestroyCount{ 0 };

Object *Find(CryptoTypes::KeyId keyId)
{
	for (auto &object : sObjects) {
		if (object.mLive && object.mId == keyId) {
			return &object;
		}
	}
	return nullptr;
}

Object *Create(Kind kind, CryptoTypes::KeyId keyId, const Material &material)
{
	for (auto &object : sObjects) {
		if (!object.mLive) {
			object = Object{ true, kind, keyId, material };
			return &object;
		}
	}
	return nullptr;
}

bool ConsumeFault(Fault fault)
{
	if (sArmedFault != fault) {
		return false;
	}
	if (sArmedSkip > 0) {
		--sArmedSkip;
		return false;
	}
	sArmedFault = Fault::None;
	return true;
}

void Release(Object &object)
{
	object.mMaterial.fill(0);
	object = Object{};
}

} // namespace

void Reset()
{
	for (auto &object : sObjects) {
		Release(object);
	}
	sNextInputKeyId = kInputKeyIdBase + 1;
	sNextTemporaryKeyId = kTemporaryKeyIdBase + 1;
	sArmedFault = Fault::None;
	sArmedSkip = 0;
	sDurableDestroyCount = 0;
	sDurableCreateCount = 0;
	sInvalidDestroyCount = 0;
}

void Reboot()
{
	for (auto &object : sObjects) {
		if (object.mLive && object.mKind != Kind::Durable) {
			Release(object);
		}
	}
	sArmedFault = Fault::None;
	sArmedSkip = 0;
}

void FailNext(Fault fault, size_t skip)
{
	sArmedFault = fault;
	sArmedSkip = skip;
}

bool FaultArmed()
{
	return sArmedFault != Fault::None;
}

CryptoTypes::KeyId CreateInputKey(const Material &material)
{
	const Object *object = Create(Kind::Input, sNextInputKeyId, material);
	if (object == nullptr) {
		return 0;
	}
	++sNextInputKeyId;
	return object->mId;
}

bool PreloadDurable(CryptoTypes::KeyId persistedKeyId, const Material &material)
{
	if (persistedKeyId == 0 || Find(persistedKeyId) != nullptr) {
		return false;
	}
	return Create(Kind::Durable, persistedKeyId, material) != nullptr;
}

bool DestroyCallerKey(CryptoTypes::KeyId keyId)
{
	Object *object = Find(keyId);
	if (object == nullptr || object->mKind == Kind::Durable) {
		++sInvalidDestroyCount;
		return false;
	}
	Release(*object);
	return true;
}

bool IsLive(CryptoTypes::KeyId keyId, Kind kind)
{
	const Object *object = Find(keyId);
	return object != nullptr && object->mKind == kind;
}

bool MaterialEquals(CryptoTypes::KeyId keyId, const Material &material)
{
	const Object *object = Find(keyId);
	return object != nullptr && object->mMaterial == material;
}

size_t LiveCount()
{
	return static_cast<size_t>(
		std::count_if(sObjects.begin(), sObjects.end(), [](const Object &object) { return object.mLive; }));
}

size_t LiveCount(Kind kind)
{
	return static_cast<size_t>(std::count_if(sObjects.begin(), sObjects.end(), [kind](const Object &object) {
		return object.mLive && object.mKind == kind;
	}));
}

size_t DurableDestroyCount()
{
	return sDurableDestroyCount;
}

size_t DurableCreateCount()
{
	return sDurableCreateCount;
}

size_t InvalidDestroyCount()
{
	return sInvalidDestroyCount;
}

} // namespace AliroUd::PersistentKey::FakePsa

namespace AliroUd::PersistentKey::Backend {

using namespace FakePsa;

AliroError DurablyOwn(CryptoTypes::KeyId sourceKeyId, CryptoTypes::KeyId desiredPersistentKeyId,
		      CryptoTypes::KeyId &outActualPersistentKeyId)
{
	outActualPersistentKeyId = 0;

	const Object *source = Find(sourceKeyId);
	if (source == nullptr || source->mKind == Kind::Durable) {
		return ALIRO_INVALID_ARGUMENT;
	}

	if (Find(desiredPersistentKeyId) != nullptr) {
		return ALIRO_KEY_ALREADY_EXISTS;
	}

	if (ConsumeFault(Fault::DurablyOwn)) {
		return ALIRO_ERROR_INTERNAL;
	}
	const FakePower::Outcome outcome =
		FakePower::NextDurableWrite(FakePower::Mutation::OwnKey, desiredPersistentKeyId);
	if (outcome == FakePower::Outcome::Refuse) {
		return ALIRO_ERROR_INTERNAL;
	}

	if (Create(Kind::Durable, desiredPersistentKeyId, source->mMaterial) == nullptr) {
		return ALIRO_NO_MEMORY;
	}
	++sDurableCreateCount;

	/* Beyond the backend contract: the key exists although the call failed. */
	if (outcome == FakePower::Outcome::LandThenFail) {
		return ALIRO_ERROR_INTERNAL;
	}

	outActualPersistentKeyId = desiredPersistentKeyId;
	return ALIRO_NO_ERROR;
}

AliroError MintVolatileHandle(CryptoTypes::KeyId persistedKeyId, CryptoTypes::KeyId &outVolatileKeyId)
{
	outVolatileKeyId = 0;

	const Object *durable = Find(persistedKeyId);
	if (durable == nullptr || durable->mKind != Kind::Durable) {
		return ALIRO_ERROR_INTERNAL;
	}

	if (ConsumeFault(Fault::MintVolatileHandle)) {
		return ALIRO_ERROR_INTERNAL;
	}

	const Object *temporary = Create(Kind::Temporary, sNextTemporaryKeyId, durable->mMaterial);
	if (temporary == nullptr) {
		return ALIRO_NO_MEMORY;
	}
	++sNextTemporaryKeyId;

	outVolatileKeyId = temporary->mId;
	return ALIRO_NO_ERROR;
}

AliroError Exists(CryptoTypes::KeyId persistedKeyId, bool &outExists)
{
	outExists = false;
	if (ConsumeFault(Fault::Exists)) {
		return ALIRO_ERROR_INTERNAL;
	}

	const Object *object = Find(persistedKeyId);
	outExists = object != nullptr && object->mKind == Kind::Durable;
	return ALIRO_NO_ERROR;
}

AliroError Destroy(CryptoTypes::KeyId persistedKeyId)
{
	Object *durable = Find(persistedKeyId);
	if (durable == nullptr || durable->mKind != Kind::Durable) {
		/* Idempotent per the backend contract, but the store under test must never rely on it. */
		++sInvalidDestroyCount;
		return ALIRO_NO_ERROR;
	}

	if (ConsumeFault(Fault::Destroy)) {
		return ALIRO_ERROR_INTERNAL;
	}
	const FakePower::Outcome outcome = FakePower::NextDurableWrite(FakePower::Mutation::DestroyKey, persistedKeyId);
	if (outcome == FakePower::Outcome::Refuse) {
		return ALIRO_ERROR_INTERNAL;
	}

	Release(*durable);
	++sDurableDestroyCount;
	return outcome == FakePower::Outcome::LandThenFail ? ALIRO_ERROR_INTERNAL : ALIRO_NO_ERROR;
}

} // namespace AliroUd::PersistentKey::Backend
