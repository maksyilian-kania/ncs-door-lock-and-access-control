/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include "recovery_support.h"

#include "fake_power_loss.h"
#include "fake_record_persistence.h"

#include <storage/key/persistent_key_persistence.h>
#include <storage/key/persistent_key_store.h>

#include <zephyr/ztest.h>

using namespace Aliro;
using namespace Aliro::UserDevice;

namespace AliroUd::PersistentKey::RecoverySupport {
namespace {

constexpr CryptoTypes::KeyId kKeyIdBase{ CONFIG_ALIRO_UD_PERSISTENT_KEY_ID_BASE };
constexpr PK::RecordHandle kPoisonRecord{ 0x5A5A5A5A };
constexpr CryptoTypes::KeyId kPoisonKeyId{ 0xA5A5A5A5 };

} // namespace

FakePsa::Material MakeMaterial(uint8_t seed)
{
	FakePsa::Material material{};
	for (size_t i = 0; i < material.size(); ++i) {
		material[i] = static_cast<uint8_t>(seed ^ (0x11 + 0x3B * i));
	}
	return material;
}

ReaderGroupSubIdentifier MakeSub(uint8_t seed)
{
	ReaderGroupSubIdentifier sub{};
	sub.fill(seed);
	return sub;
}

CryptoTypes::KeyId CommittedKeyId(size_t slotIndex)
{
	return kKeyIdBase + static_cast<CryptoTypes::KeyId>(2 * slotIndex);
}

CryptoTypes::KeyId StagedKeyId(size_t slotIndex)
{
	return CommittedKeyId(slotIndex) + 1;
}

Record MakeRecord(PK::RecordHandle handle, CredentialHandle credential, const ReaderGroupSubIdentifier &sub,
		  CryptoTypes::KeyId keyId)
{
	Record record{};
	record.mValid = true;
	record.mHandle = handle;
	record.mCredentialHandle = credential;
	record.mReaderGroupSubIdentifier = sub;
	record.mPersistedKeyId = keyId;
	return record;
}

void ClearAll()
{
	FakePower::Reset();
	FakePsa::Reset();
	FakeStorage::Reset();
}

AliroError Reboot()
{
	FakePower::Reset();
	FakePsa::Reboot();
	return Store::Init();
}

Stored Insert(CredentialHandle credential, const ReaderGroupSubIdentifier &sub, uint8_t seed)
{
	Stored stored{};
	stored.mMaterial = MakeMaterial(seed);

	const CryptoTypes::KeyId input = FakePsa::CreateInputKey(stored.mMaterial);
	zassert_not_equal(0u, input, "fake PSA table must have room for an input key");

	zassert_equal(ALIRO_NO_ERROR, PK::Replace(credential, sub, input, stored.mRecord), "Replace must succeed");
	zassert_not_equal(PK::kInvalidRecordHandle, stored.mRecord, "a committed record needs a valid handle");
	zassert_true(FakePsa::DestroyCallerKey(input), "the caller must be able to release its input key");

	return stored;
}

void ExpectResolves(CredentialHandle credential, const ReaderGroupSubIdentifier &sub, const Stored &stored)
{
	zassert_true(ResolvesOrMisses(credential, sub, stored), "Lookup must hit");
}

void ExpectMiss(CredentialHandle credential, const ReaderGroupSubIdentifier &sub)
{
	PK::RecordHandle record{ kPoisonRecord };
	CryptoTypes::KeyId temporary{ kPoisonKeyId };

	zassert_equal(ALIRO_ERROR_UNKNOWN, PK::Lookup(credential, sub, record, temporary), "Lookup must miss");
	zassert_equal(PK::kInvalidRecordHandle, record);
	zassert_equal(0u, temporary);
}

bool ResolvesOrMisses(CredentialHandle credential, const ReaderGroupSubIdentifier &sub, const Stored &stored)
{
	PK::RecordHandle record{ kPoisonRecord };
	CryptoTypes::KeyId temporary{ kPoisonKeyId };

	const AliroError error = PK::Lookup(credential, sub, record, temporary);
	if (error == ALIRO_ERROR_UNKNOWN) {
		zassert_equal(PK::kInvalidRecordHandle, record);
		zassert_equal(0u, temporary);
		return false;
	}

	zassert_equal(ALIRO_NO_ERROR, error, "Lookup must either hit or miss");
	zassert_equal(stored.mRecord, record, "Lookup must return the committed record handle");
	zassert_true(FakePsa::MaterialEquals(temporary, stored.mMaterial),
		     "the temporary handle must wrap the committed Kpersistent material");
	zassert_true(FakePsa::DestroyCallerKey(temporary));
	return true;
}

void ExpectConsistent()
{
	size_t present{ 0 };

	for (size_t i = 0; i < kMaxRecords; ++i) {
		Record record{};
		bool isPresent{ false };
		zassert_equal(ALIRO_NO_ERROR, Persistence::LoadRecord(i, record, isPresent));
		if (!isPresent) {
			continue;
		}
		++present;

		zassert_true(record.mValid, "slot %zu: a persisted record must be valid", i);
		zassert_true(record.mPersistedKeyId == CommittedKeyId(i) || record.mPersistedKeyId == StagedKeyId(i),
			     "slot %zu: the record must reference one of its slot's key IDs", i);
		zassert_true(FakePsa::IsLive(record.mPersistedKeyId, FakePsa::Kind::Durable),
			     "slot %zu: the record's durable key must exist", i);

		PK::RecordHandle handle{};
		CryptoTypes::KeyId temporary{};
		zassert_equal(ALIRO_NO_ERROR,
			      PK::Lookup(record.mCredentialHandle, record.mReaderGroupSubIdentifier, handle, temporary),
			      "slot %zu: a persisted record must resolve", i);
		zassert_equal(record.mHandle, handle);
		zassert_true(FakePsa::DestroyCallerKey(temporary));
	}

	zassert_equal(present, FakePsa::LiveCount(FakePsa::Kind::Durable), "no durable key may be unowned");
	zassert_equal(0u, FakePsa::InvalidDestroyCount(), "no key object may be destroyed twice or while absent");
}

void ExpectStableReboot()
{
	const FakeStorage::Image image = FakeStorage::Capture();
	const size_t writes = FakeStorage::WriteCount();
	const size_t creates = FakePsa::DurableCreateCount();
	const size_t destroys = FakePsa::DurableDestroyCount();
	const size_t durable = FakePsa::LiveCount(FakePsa::Kind::Durable);

	zassert_equal(ALIRO_NO_ERROR, Reboot(), "a repeated boot must succeed");
	zassert_true(FakeStorage::Capture() == image, "a repeated boot must not change persisted records");
	zassert_equal(writes, FakeStorage::WriteCount(), "a repeated boot must not write");
	zassert_equal(creates, FakePsa::DurableCreateCount(), "a repeated boot must not create a key");
	zassert_equal(destroys, FakePsa::DurableDestroyCount(), "a repeated boot must not destroy a key");
	zassert_equal(durable, FakePsa::LiveCount(FakePsa::Kind::Durable));
	ExpectConsistent();
}

void ExpectCleanTeardown()
{
	zassert_equal(ALIRO_NO_ERROR, PK::Reset(), "factory reset must succeed");
	zassert_equal(0u, FakeStorage::PresentCount(), "no persisted record may survive factory reset");
	zassert_equal(0u, FakePsa::LiveCount(), "every fake PSA object must be released");
	zassert_equal(0u, FakePsa::InvalidDestroyCount(), "no key object may be destroyed twice or while absent");
}

} // namespace AliroUd::PersistentKey::RecoverySupport
