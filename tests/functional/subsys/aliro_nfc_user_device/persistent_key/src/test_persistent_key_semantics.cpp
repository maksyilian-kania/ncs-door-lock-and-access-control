/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include <zephyr/ztest.h>

#include "fake_psa_key_backend.h"
#include "fake_record_persistence.h"
#include "storage/key/persistent_key_store.h"
#include "storage/key/persistent_key_types.h"

#include <aliro/user_device/interface.h>

#include <algorithm>

/*
 * Direct semantics of the application Kpersistent backend, independent of
 * protocol flow: exact {CredentialHandle, reader_group_sub_identifier}
 * lookup (Aliro 1.0 Specification, section 6.2, page 28; section 8.3.3.4,
 * page 72), replacement, capacity, deletion, and ownership of every PSA key
 * object. Each test ends with zero live fake PSA objects and zero invalid
 * destroys. Assertion messages never format key material.
 */

using namespace Aliro;
using namespace Aliro::UserDevice;

namespace PK = ::Aliro::Interface::UserDevice::PersistentKey;
namespace Store = ::AliroUd::PersistentKey::Store;
namespace FakePsa = ::AliroUd::PersistentKey::FakePsa;
namespace FakeStorage = ::AliroUd::PersistentKey::FakeStorage;

using FakePsa::Kind;
using FakePsa::Material;

namespace {

constexpr size_t kPerCredential{ AliroUd::PersistentKey::kMaxRecordsPerCredential };
constexpr size_t kGlobal{ AliroUd::PersistentKey::kMaxRecords };
constexpr size_t kCredentials{ CONFIG_ALIRO_UD_MAX_CREDENTIALS };
constexpr CryptoTypes::KeyId kKeyIdBase{ CONFIG_ALIRO_UD_PERSISTENT_KEY_ID_BASE };

constexpr CredentialHandle kCredentialA{ 1 };
constexpr CredentialHandle kCredentialB{ 2 };

constexpr PK::RecordHandle kPoisonRecord{ 0x5A5A5A5A };
constexpr CryptoTypes::KeyId kPoisonKeyId{ 0xA5A5A5A5 };

struct Stored {
	PK::RecordHandle mRecord{ PK::kInvalidRecordHandle };
	Material mMaterial{};
};

Material MakeMaterial(uint8_t seed)
{
	Material material{};
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

CredentialHandle CredentialAt(size_t index)
{
	return static_cast<CredentialHandle>(index + 1);
}

/* Commits one record, then releases the input key as the stack does after Replace(). */
Stored Insert(CredentialHandle credential, const ReaderGroupSubIdentifier &sub, uint8_t seed)
{
	Stored stored{};
	stored.mMaterial = MakeMaterial(seed);

	const CryptoTypes::KeyId input = FakePsa::CreateInputKey(stored.mMaterial);
	zassert_not_equal(0u, input, "fake PSA table must have room for an input key");

	zassert_equal(ALIRO_NO_ERROR, PK::Replace(credential, sub, input, stored.mRecord), "Replace must succeed");
	zassert_not_equal(PK::kInvalidRecordHandle, stored.mRecord, "a committed record needs a valid handle");
	zassert_true(FakePsa::IsLive(input, Kind::Input), "Replace must leave the caller's input key alive");
	zassert_true(FakePsa::DestroyCallerKey(input), "the caller must be able to release its input key");

	return stored;
}

void FillCredential(CredentialHandle credential, size_t count)
{
	for (size_t i = 0; i < count; ++i) {
		(void)Insert(credential, MakeSub(static_cast<uint8_t>(i)), static_cast<uint8_t>(credential * 64 + i));
	}
}

void ExpectResolves(CredentialHandle credential, const ReaderGroupSubIdentifier &sub, const Stored &stored)
{
	PK::RecordHandle record{ kPoisonRecord };
	CryptoTypes::KeyId temporary{ kPoisonKeyId };

	zassert_equal(ALIRO_NO_ERROR, PK::Lookup(credential, sub, record, temporary), "Lookup must hit");
	zassert_equal(stored.mRecord, record, "Lookup must return the committed record handle");
	zassert_true(FakePsa::IsLive(temporary, Kind::Temporary), "Lookup must mint a fresh temporary handle");
	zassert_true(FakePsa::MaterialEquals(temporary, stored.mMaterial),
		     "the temporary handle must wrap the committed Kpersistent material");
	zassert_true(FakePsa::DestroyCallerKey(temporary), "the caller owns and releases the temporary handle");
}

void ExpectMiss(CredentialHandle credential, const ReaderGroupSubIdentifier &sub)
{
	PK::RecordHandle record{ kPoisonRecord };
	CryptoTypes::KeyId temporary{ kPoisonKeyId };
	const size_t liveBefore = FakePsa::LiveCount();

	zassert_equal(ALIRO_ERROR_UNKNOWN, PK::Lookup(credential, sub, record, temporary), "Lookup must miss");
	zassert_equal(PK::kInvalidRecordHandle, record, "a miss must leave outRecord invalid");
	zassert_equal(0u, temporary, "a miss must leave outKeyId zero");
	zassert_equal(liveBefore, FakePsa::LiveCount(), "a miss must not create any key object");
}

struct CommittedState {
	FakeStorage::Image mImage{};
	size_t mWrites{ 0 };
	size_t mDurable{ 0 };
};

CommittedState CaptureCommitted()
{
	return { FakeStorage::Capture(), FakeStorage::WriteCount(), FakePsa::LiveCount(Kind::Durable) };
}

void ExpectCommittedUnchanged(const CommittedState &before)
{
	zassert_true(FakeStorage::Capture() == before.mImage, "persisted records must be unchanged");
	zassert_equal(before.mWrites, FakeStorage::WriteCount(), "no persistence write may occur");
	zassert_equal(before.mDurable, FakePsa::LiveCount(Kind::Durable), "no durable key may be added or removed");
}

/* Replace() that must fail with `expected` while leaving committed state and the input key untouched. */
void ExpectReplaceRejected(CredentialHandle credential, const ReaderGroupSubIdentifier &sub, AliroError expected)
{
	const CryptoTypes::KeyId input = FakePsa::CreateInputKey(MakeMaterial(0xEE));
	const CommittedState before = CaptureCommitted();
	PK::RecordHandle record{ kPoisonRecord };

	zassert_equal(expected, PK::Replace(credential, sub, input, record), "Replace must be rejected");
	zassert_equal(PK::kInvalidRecordHandle, record, "a rejected Replace must leave outRecord invalid");
	ExpectCommittedUnchanged(before);
	zassert_true(FakePsa::IsLive(input, Kind::Input), "a rejected Replace must leave the input key alive");
	zassert_true(FakePsa::DestroyCallerKey(input));
}

void ExpectCleanTeardown()
{
	zassert_equal(ALIRO_NO_ERROR, PK::Reset(), "factory reset must succeed");
	zassert_equal(0u, FakeStorage::PresentCount(), "no persisted record may survive factory reset");
	zassert_equal(0u, FakePsa::LiveCount(), "every fake PSA object must be released");
	zassert_equal(0u, FakePsa::InvalidDestroyCount(), "no key object may be destroyed twice or while absent");
}

void ResetBeforeEachTest(void *fixture)
{
	(void)fixture;

	FakePsa::Reset();
	FakeStorage::Reset();
	zassert_equal(ALIRO_NO_ERROR, Store::Init(), "store init must succeed on empty storage");
}

} // namespace

ZTEST_SUITE(aliro_ud_persistent_key_semantics, nullptr, nullptr, ResetBeforeEachTest, nullptr, nullptr);

ZTEST(aliro_ud_persistent_key_semantics, test_lookup_miss_initializes_outputs)
{
	ExpectMiss(kCredentialA, MakeSub(0x10));
	ExpectMiss(kInvalidCredentialHandle, MakeSub(0x10));

	const Stored stored = Insert(kCredentialA, MakeSub(0x10), 0x01);
	ExpectMiss(kInvalidCredentialHandle, MakeSub(0x10));
	ExpectResolves(kCredentialA, MakeSub(0x10), stored);

	ExpectCleanTeardown();
}

ZTEST(aliro_ud_persistent_key_semantics, test_lookup_matches_exact_pair_only)
{
	const ReaderGroupSubIdentifier sub = MakeSub(0x20);
	const Stored stored = Insert(kCredentialA, sub, 0x02);

	ReaderGroupSubIdentifier firstByte = sub;
	firstByte.front() ^= 0x01;
	ReaderGroupSubIdentifier lastByte = sub;
	lastByte.back() ^= 0x80;

	ExpectResolves(kCredentialA, sub, stored);
	ExpectMiss(kCredentialA, firstByte);
	ExpectMiss(kCredentialA, lastByte);
	ExpectMiss(kCredentialB, sub);

	if (kGlobal >= 2) {
		/* The same sub-identifier under another credential is an independent record. */
		const Stored other = Insert(kCredentialB, sub, 0x03);
		zassert_not_equal(stored.mRecord, other.mRecord, "distinct pairs need distinct handles");
		ExpectResolves(kCredentialA, sub, stored);
		ExpectResolves(kCredentialB, sub, other);
	}

	ExpectCleanTeardown();
}

ZTEST(aliro_ud_persistent_key_semantics, test_lookup_mints_independent_caller_owned_handles)
{
	const ReaderGroupSubIdentifier sub = MakeSub(0x30);
	const Stored stored = Insert(kCredentialA, sub, 0x04);

	PK::RecordHandle record{};
	CryptoTypes::KeyId first{};
	CryptoTypes::KeyId second{};
	zassert_equal(ALIRO_NO_ERROR, PK::Lookup(kCredentialA, sub, record, first));
	zassert_equal(ALIRO_NO_ERROR, PK::Lookup(kCredentialA, sub, record, second));
	zassert_not_equal(first, second, "each Lookup must mint a new temporary handle");
	zassert_equal(2u, FakePsa::LiveCount(Kind::Temporary));

	/* Releasing one temporary affects neither the other nor the durable record. */
	zassert_true(FakePsa::DestroyCallerKey(first));
	zassert_true(FakePsa::MaterialEquals(second, stored.mMaterial));
	ExpectResolves(kCredentialA, sub, stored);

	/* The store never releases a temporary, even when its record is deleted. */
	zassert_equal(ALIRO_NO_ERROR, PK::Delete(stored.mRecord));
	zassert_true(FakePsa::IsLive(second, Kind::Temporary), "deleting a record must not destroy a caller's handle");
	zassert_true(FakePsa::DestroyCallerKey(second));
	ExpectMiss(kCredentialA, sub);

	ExpectCleanTeardown();
}

ZTEST(aliro_ud_persistent_key_semantics, test_lookup_mint_failure_initializes_outputs)
{
	const ReaderGroupSubIdentifier sub = MakeSub(0x38);
	const Stored stored = Insert(kCredentialA, sub, 0x05);
	const CommittedState before = CaptureCommitted();

	PK::RecordHandle record{ kPoisonRecord };
	CryptoTypes::KeyId temporary{ kPoisonKeyId };
	FakePsa::FailNext(FakePsa::Fault::MintVolatileHandle);
	zassert_not_equal(ALIRO_NO_ERROR, PK::Lookup(kCredentialA, sub, record, temporary));
	zassert_equal(PK::kInvalidRecordHandle, record, "a failed Lookup must leave outRecord invalid");
	zassert_equal(0u, temporary, "a failed Lookup must leave outKeyId zero");
	zassert_equal(0u, FakePsa::LiveCount(Kind::Temporary), "a failed mint must not leave a temporary object");
	ExpectCommittedUnchanged(before);

	ExpectResolves(kCredentialA, sub, stored);
	ExpectCleanTeardown();
}

ZTEST(aliro_ud_persistent_key_semantics, test_first_insert_owns_one_durable_copy)
{
	const ReaderGroupSubIdentifier sub = MakeSub(0x40);
	const Material material = MakeMaterial(0x06);
	const CryptoTypes::KeyId input = FakePsa::CreateInputKey(material);

	PK::RecordHandle record{ kPoisonRecord };
	zassert_equal(ALIRO_NO_ERROR, PK::Replace(kCredentialA, sub, input, record));
	zassert_not_equal(PK::kInvalidRecordHandle, record);
	zassert_true(FakePsa::IsLive(input, Kind::Input), "the input key stays caller-owned");
	zassert_true(FakePsa::MaterialEquals(input, material), "the input key must be left untouched");
	zassert_equal(1u, FakePsa::LiveCount(Kind::Durable), "exactly one durable copy per committed record");
	zassert_true(FakePsa::IsLive(kKeyIdBase, Kind::Durable), "slot 0 commits at its first persistent key ID");
	zassert_equal(1u, FakeStorage::PresentCount());

	/* The durable copy is independent of the caller's input key. */
	zassert_true(FakePsa::DestroyCallerKey(input));
	ExpectResolves(kCredentialA, sub, Stored{ record, material });

	ExpectCleanTeardown();
}

ZTEST(aliro_ud_persistent_key_semantics, test_same_pair_replacement_retires_old_copy)
{
	const ReaderGroupSubIdentifier sub = MakeSub(0x50);
	const Stored first = Insert(kCredentialA, sub, 0x07);
	zassert_true(FakePsa::IsLive(kKeyIdBase, Kind::Durable));

	const Stored second = Insert(kCredentialA, sub, 0x08);
	zassert_equal(first.mRecord, second.mRecord, "replacement keeps the record handle");
	zassert_equal(1u, FakePsa::LiveCount(Kind::Durable), "replacement must retire the old durable copy");
	zassert_true(FakePsa::IsLive(kKeyIdBase + 1, Kind::Durable), "replacement commits at the staged key ID");
	zassert_equal(1u, FakePsa::DurableDestroyCount());
	zassert_equal(1u, FakeStorage::PresentCount());
	ExpectResolves(kCredentialA, sub, second);

	const Stored third = Insert(kCredentialA, sub, 0x09);
	zassert_equal(first.mRecord, third.mRecord);
	zassert_equal(1u, FakePsa::LiveCount(Kind::Durable));
	zassert_true(FakePsa::IsLive(kKeyIdBase, Kind::Durable), "a second replacement alternates back");
	zassert_equal(2u, FakePsa::DurableDestroyCount());
	ExpectResolves(kCredentialA, sub, third);

	ExpectCleanTeardown();
}

ZTEST(aliro_ud_persistent_key_semantics, test_replace_rejects_invalid_inputs)
{
	const ReaderGroupSubIdentifier sub = MakeSub(0x60);
	const Stored stored = Insert(kCredentialA, sub, 0x0A);

	ExpectReplaceRejected(kInvalidCredentialHandle, sub, ALIRO_INVALID_ARGUMENT);

	const CommittedState before = CaptureCommitted();
	PK::RecordHandle record{ kPoisonRecord };
	zassert_equal(ALIRO_INVALID_ARGUMENT, PK::Replace(kCredentialA, sub, 0, record), "a zero keyId is invalid");
	zassert_equal(PK::kInvalidRecordHandle, record);

	record = kPoisonRecord;
	zassert_equal(ALIRO_INVALID_ARGUMENT, PK::Replace(kCredentialA, sub, 0x4000FFFF, record),
		      "a keyId naming no live key is invalid");
	zassert_equal(PK::kInvalidRecordHandle, record);
	ExpectCommittedUnchanged(before);

	ExpectResolves(kCredentialA, sub, stored);
	ExpectCleanTeardown();
}

ZTEST(aliro_ud_persistent_key_semantics, test_replace_copy_failure_preserves_committed_state)
{
	const ReaderGroupSubIdentifier sub = MakeSub(0x70);
	const Stored stored = Insert(kCredentialA, sub, 0x0B);

	/* Existing pair: the old record and its durable copy survive. */
	FakePsa::FailNext(FakePsa::Fault::DurablyOwn);
	ExpectReplaceRejected(kCredentialA, sub, ALIRO_ERROR_INTERNAL);
	ExpectResolves(kCredentialA, sub, stored);

	if (kPerCredential >= 2) {
		/* New pair: nothing is committed. */
		FakePsa::FailNext(FakePsa::Fault::DurablyOwn);
		ExpectReplaceRejected(kCredentialA, MakeSub(0x71), ALIRO_ERROR_INTERNAL);
		ExpectMiss(kCredentialA, MakeSub(0x71));
	}

	ExpectCleanTeardown();
}

ZTEST(aliro_ud_persistent_key_semantics, test_replace_persist_failure_destroys_new_copy)
{
	const ReaderGroupSubIdentifier sub = MakeSub(0x80);
	const Stored stored = Insert(kCredentialA, sub, 0x0C);
	const size_t destroysBefore = FakePsa::DurableDestroyCount();

	/* Existing pair: the staged copy is destroyed; the committed one survives. */
	FakeStorage::FailNextSave();
	ExpectReplaceRejected(kCredentialA, sub, ALIRO_ERROR_INTERNAL);
	zassert_equal(destroysBefore + 1, FakePsa::DurableDestroyCount(), "the uncommitted copy must be destroyed");
	zassert_true(FakePsa::IsLive(kKeyIdBase, Kind::Durable), "the committed copy must survive");
	ExpectResolves(kCredentialA, sub, stored);

	/* A retry after the failure reuses the staged key ID without collision. */
	const Stored replaced = Insert(kCredentialA, sub, 0x0D);
	zassert_equal(stored.mRecord, replaced.mRecord);
	ExpectResolves(kCredentialA, sub, replaced);

	if (kPerCredential >= 2) {
		FakeStorage::FailNextSave();
		ExpectReplaceRejected(kCredentialA, MakeSub(0x81), ALIRO_ERROR_INTERNAL);
		ExpectMiss(kCredentialA, MakeSub(0x81));
	}

	ExpectCleanTeardown();
}

ZTEST(aliro_ud_persistent_key_semantics, test_per_credential_capacity)
{
	FillCredential(kCredentialA, kPerCredential);
	zassert_equal(kPerCredential, FakeStorage::PresentCount());

	ExpectReplaceRejected(kCredentialA, MakeSub(0xF0), ALIRO_NO_MEMORY);
	ExpectMiss(kCredentialA, MakeSub(0xF0));

	/* Replacing an existing pair at capacity still succeeds. */
	const Stored replaced = Insert(kCredentialA, MakeSub(0), 0xA0);
	ExpectResolves(kCredentialA, MakeSub(0), replaced);
	zassert_equal(kPerCredential, FakePsa::LiveCount(Kind::Durable));

	if (kCredentials >= 2) {
		/* The limit is per credential: another credential is unaffected. */
		const Stored other = Insert(kCredentialB, MakeSub(0xF0), 0xA1);
		ExpectResolves(kCredentialB, MakeSub(0xF0), other);
	}

	ExpectCleanTeardown();
}

ZTEST(aliro_ud_persistent_key_semantics, test_global_capacity)
{
	for (size_t i = 0; i < kCredentials; ++i) {
		FillCredential(CredentialAt(i), kPerCredential);
	}
	zassert_equal(kGlobal, FakeStorage::PresentCount());
	zassert_equal(kGlobal, FakePsa::LiveCount(Kind::Durable));

	const CredentialHandle extra = CredentialAt(kCredentials);
	ExpectReplaceRejected(extra, MakeSub(0), ALIRO_NO_MEMORY);
	ExpectMiss(extra, MakeSub(0));

	/* Freeing one record makes exactly one slot reusable, without a key-ID collision. */
	PK::RecordHandle record{};
	CryptoTypes::KeyId temporary{};
	zassert_equal(ALIRO_NO_ERROR, PK::Lookup(kCredentialA, MakeSub(0), record, temporary));
	zassert_true(FakePsa::DestroyCallerKey(temporary));
	zassert_equal(ALIRO_NO_ERROR, PK::Delete(record));

	const Stored reused = Insert(extra, MakeSub(0), 0xB0);
	ExpectResolves(extra, MakeSub(0), reused);
	ExpectReplaceRejected(extra, MakeSub(1), ALIRO_NO_MEMORY);

	ExpectCleanTeardown();
}

ZTEST(aliro_ud_persistent_key_semantics, test_delete_single_record)
{
	const Stored stored = Insert(kCredentialA, MakeSub(0x90), 0x0E);
	Stored sibling{};
	if (kPerCredential >= 2) {
		sibling = Insert(kCredentialA, MakeSub(0x91), 0x0F);
	}
	const size_t durableBefore = FakePsa::LiveCount(Kind::Durable);

	zassert_equal(ALIRO_NO_ERROR, PK::Delete(stored.mRecord));
	ExpectMiss(kCredentialA, MakeSub(0x90));
	zassert_equal(durableBefore - 1, FakePsa::LiveCount(Kind::Durable), "Delete must destroy the record's copy");
	zassert_equal(1u, FakePsa::DurableDestroyCount());
	if (kPerCredential >= 2) {
		ExpectResolves(kCredentialA, MakeSub(0x91), sibling);
	}

	/* Already-removed, invalid, and unknown handles are no-ops. */
	const CommittedState before = CaptureCommitted();
	zassert_equal(ALIRO_NO_ERROR, PK::Delete(stored.mRecord));
	zassert_equal(ALIRO_NO_ERROR, PK::Delete(PK::kInvalidRecordHandle));
	zassert_equal(ALIRO_NO_ERROR, PK::Delete(0x0000FFFF));
	ExpectCommittedUnchanged(before);
	zassert_equal(1u, FakePsa::DurableDestroyCount());

	ExpectCleanTeardown();
}

ZTEST(aliro_ud_persistent_key_semantics, test_deleted_handle_is_not_reused)
{
	const ReaderGroupSubIdentifier sub = MakeSub(0xA0);
	const Stored first = Insert(kCredentialA, sub, 0x10);
	zassert_equal(ALIRO_NO_ERROR, PK::Delete(first.mRecord));

	const Stored second = Insert(kCredentialA, sub, 0x11);
	zassert_not_equal(first.mRecord, second.mRecord, "a deleted handle must not name a new record");

	/* A stale Delete() of the old handle leaves the new record alone. */
	zassert_equal(ALIRO_NO_ERROR, PK::Delete(first.mRecord));
	ExpectResolves(kCredentialA, sub, second);

	ExpectCleanTeardown();
}

ZTEST(aliro_ud_persistent_key_semantics, test_delete_all_for_credential)
{
	const size_t ownCount = std::min<size_t>(kPerCredential, 2);
	FillCredential(kCredentialA, ownCount);

	const bool hasOther = kCredentials >= 2;
	Stored other{};
	if (hasOther) {
		other = Insert(kCredentialB, MakeSub(0), 0xC0);
	}

	zassert_equal(ALIRO_NO_ERROR, Store::DeleteAllForCredential(kCredentialA));
	for (size_t i = 0; i < ownCount; ++i) {
		ExpectMiss(kCredentialA, MakeSub(static_cast<uint8_t>(i)));
	}
	zassert_equal(ownCount, FakePsa::DurableDestroyCount(), "every record of the credential is destroyed");
	if (hasOther) {
		ExpectResolves(kCredentialB, MakeSub(0), other);
	}

	/* Repeated, unknown, and invalid credentials are no-ops. */
	const CommittedState before = CaptureCommitted();
	zassert_equal(ALIRO_NO_ERROR, Store::DeleteAllForCredential(kCredentialA));
	zassert_equal(ALIRO_NO_ERROR, Store::DeleteAllForCredential(CredentialAt(kCredentials + 1)));
	zassert_equal(ALIRO_NO_ERROR, Store::DeleteAllForCredential(kInvalidCredentialHandle));
	ExpectCommittedUnchanged(before);

	ExpectCleanTeardown();
}

ZTEST(aliro_ud_persistent_key_semantics, test_factory_reset)
{
	for (size_t i = 0; i < kCredentials; ++i) {
		FillCredential(CredentialAt(i), kPerCredential);
	}

	/* A temporary handle held across Reset() stays caller-owned. */
	PK::RecordHandle record{};
	CryptoTypes::KeyId temporary{};
	zassert_equal(ALIRO_NO_ERROR, PK::Lookup(kCredentialA, MakeSub(0), record, temporary));

	zassert_equal(ALIRO_NO_ERROR, PK::Reset());
	zassert_equal(0u, FakeStorage::PresentCount());
	zassert_equal(0u, FakePsa::LiveCount(Kind::Durable));
	zassert_equal(kGlobal, FakePsa::DurableDestroyCount(), "each durable copy is destroyed exactly once");
	ExpectMiss(kCredentialA, MakeSub(0));
	zassert_true(FakePsa::IsLive(temporary, Kind::Temporary));
	zassert_true(FakePsa::DestroyCallerKey(temporary));

	/* Resetting an empty store is a no-op. */
	const CommittedState before = CaptureCommitted();
	zassert_equal(ALIRO_NO_ERROR, PK::Reset());
	ExpectCommittedUnchanged(before);

	/* Every slot is reusable after reset. */
	const Stored fresh = Insert(kCredentialA, MakeSub(0), 0xD0);
	zassert_not_equal(record, fresh.mRecord, "reset must not recycle a previous handle");
	ExpectResolves(kCredentialA, MakeSub(0), fresh);

	ExpectCleanTeardown();
}

ZTEST(aliro_ud_persistent_key_semantics, test_handle_allocation_skips_invalid_and_live_handles)
{
	constexpr PK::RecordHandle kTopHandle{ 0xFFFFFFFF };
	const Material topMaterial = MakeMaterial(0x12);

	/* The top handle is loaded last, so Init() leaves the next handle wrapped to 0. */
	const bool withLiveLowHandle = kGlobal >= 3 && kPerCredential >= 3;
	const size_t topSlot = withLiveLowHandle ? 1 : 0;

	AliroUd::PersistentKey::Record top{};
	top.mValid = true;
	top.mHandle = kTopHandle;
	top.mCredentialHandle = kCredentialA;
	top.mReaderGroupSubIdentifier = MakeSub(0xB0);
	top.mPersistedKeyId = kKeyIdBase + static_cast<CryptoTypes::KeyId>(2 * topSlot);
	FakeStorage::Preload(topSlot, top);
	zassert_true(FakePsa::PreloadDurable(top.mPersistedKeyId, topMaterial));

	Stored low{};
	if (withLiveLowHandle) {
		AliroUd::PersistentKey::Record lowRecord = top;
		lowRecord.mHandle = 1;
		lowRecord.mReaderGroupSubIdentifier = MakeSub(0xB1);
		lowRecord.mPersistedKeyId = kKeyIdBase;
		FakeStorage::Preload(0, lowRecord);
		low = Stored{ 1, MakeMaterial(0x13) };
		zassert_true(FakePsa::PreloadDurable(kKeyIdBase, low.mMaterial));
	}

	zassert_equal(ALIRO_NO_ERROR, Store::Init(), "reboot onto preloaded records must succeed");
	ExpectResolves(kCredentialA, MakeSub(0xB0), Stored{ kTopHandle, topMaterial });

	if (withLiveLowHandle) {
		const Stored next = Insert(kCredentialA, MakeSub(0xB2), 0x14);
		zassert_equal(2u, next.mRecord, "allocation after wrap must skip 0 and the live handle 1");
		ExpectResolves(kCredentialA, MakeSub(0xB1), low);
	} else {
		zassert_equal(ALIRO_NO_ERROR, PK::Delete(kTopHandle));
		const Stored next = Insert(kCredentialA, MakeSub(0xB2), 0x14);
		zassert_not_equal(PK::kInvalidRecordHandle, next.mRecord, "allocation after wrap must skip 0");
		zassert_not_equal(kTopHandle, next.mRecord);
		zassert_equal(ALIRO_NO_ERROR, PK::Delete(next.mRecord));
		ExpectMiss(kCredentialA, MakeSub(0xB2));
	}

	ExpectCleanTeardown();
}

ZTEST(aliro_ud_persistent_key_semantics, test_no_raw_kpersistent_in_persisted_records)
{
	const size_t count = std::min<size_t>(kGlobal, 4);
	std::array<Stored, 4> stored{};
	for (size_t i = 0; i < count; ++i) {
		stored[i] = Insert(CredentialAt(i % kCredentials), MakeSub(static_cast<uint8_t>(0xC0 + i)),
				   static_cast<uint8_t>(0x20 + i));
	}

	constexpr size_t kWindow{ 8 };
	for (size_t i = 0; i < count; ++i) {
		const Material &material = stored[i].mMaterial;
		for (size_t offset = 0; offset + kWindow <= material.size(); ++offset) {
			zassert_false(FakeStorage::ImageContains(material.data() + offset, kWindow),
				      "persisted records must not contain Kpersistent bytes (record %zu)", i);
		}
	}

	ExpectCleanTeardown();
}
