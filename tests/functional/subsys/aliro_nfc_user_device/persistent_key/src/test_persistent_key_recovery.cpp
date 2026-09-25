/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include <zephyr/ztest.h>

#include "fake_power_loss.h"
#include "fake_psa_key_backend.h"
#include "fake_record_persistence.h"
#include "recovery_support.h"
#include "storage/key/persistent_key_store.h"

#include <algorithm>
#include <array>

/*
 * Crash-safe persistence and boot recovery of the application Kpersistent
 * store. Kpersistent is a long-term key kept in non-volatile memory (Aliro
 * 1.0 Specification, section 3.1, page 23; section 8.3.1.13, page 59);
 * its lifecycle management is implementation-specific (section 6.2,
 * page 28). Every durable state is either preloaded directly or reached by
 * cutting power after each durable write, and every test checks that one
 * reboot restores the recovered invariant and that a second reboot changes
 * nothing.
 */

using namespace Aliro;
using namespace Aliro::UserDevice;
using namespace AliroUd::PersistentKey::RecoverySupport;

namespace Store = ::AliroUd::PersistentKey::Store;
namespace FakePsa = ::AliroUd::PersistentKey::FakePsa;
namespace FakeStorage = ::AliroUd::PersistentKey::FakeStorage;
namespace FakePower = ::AliroUd::PersistentKey::FakePower;

using AliroUd::PersistentKey::Record;
using FakePsa::Kind;

namespace {

constexpr size_t kPerCredential{ AliroUd::PersistentKey::kMaxRecordsPerCredential };
constexpr size_t kGlobal{ AliroUd::PersistentKey::kMaxRecords };
constexpr size_t kCredentials{ CONFIG_ALIRO_UD_MAX_CREDENTIALS };

constexpr CredentialHandle kCredentialA{ 1 };
constexpr CredentialHandle kCredentialB{ 2 };

CredentialHandle CredentialAt(size_t index)
{
	return static_cast<CredentialHandle>(index % kCredentials + 1);
}

void ColdBoot()
{
	ClearAll();
	zassert_equal(ALIRO_NO_ERROR, Store::Init(), "store init must succeed on empty storage");
}

void BootOntoPreloadedState()
{
	zassert_equal(ALIRO_NO_ERROR, Reboot(), "recovery must succeed");
	ExpectConsistent();
	ExpectStableReboot();
}

void ExpectReplaceRejected(CredentialHandle credential, const ReaderGroupSubIdentifier &sub)
{
	const CryptoTypes::KeyId input = FakePsa::CreateInputKey(MakeMaterial(0xEE));
	const FakeStorage::Image image = FakeStorage::Capture();
	const size_t writes = FakeStorage::WriteCount();
	const size_t durable = FakePsa::LiveCount(Kind::Durable);
	PK::RecordHandle record{ 0x5A5A5A5A };

	zassert_not_equal(ALIRO_NO_ERROR, PK::Replace(credential, sub, input, record), "Replace must fail");
	zassert_equal(PK::kInvalidRecordHandle, record);
	zassert_true(FakeStorage::Capture() == image, "persisted records must be unchanged");
	zassert_equal(writes, FakeStorage::WriteCount());
	zassert_equal(durable, FakePsa::LiveCount(Kind::Durable));
	zassert_true(FakePsa::DestroyCallerKey(input));
}

void ResetBeforeEachTest(void *fixture)
{
	(void)fixture;
	ColdBoot();
}

} // namespace

ZTEST_SUITE(aliro_ud_persistent_key_recovery, nullptr, nullptr, ResetBeforeEachTest, nullptr, nullptr);

/* Committed records and their keys survive reboot unchanged. */
ZTEST(aliro_ud_persistent_key_recovery, test_boot_keeps_committed_records)
{
	const Stored first = Insert(kCredentialA, MakeSub(0x10), 0x01);
	Stored second{};
	if (kGlobal >= 2) {
		second = Insert(CredentialAt(1), MakeSub(0x11), 0x02);
	}

	ExpectStableReboot();
	ExpectResolves(kCredentialA, MakeSub(0x10), first);
	if (kGlobal >= 2) {
		ExpectResolves(CredentialAt(1), MakeSub(0x11), second);
	}

	ExpectCleanTeardown();
}

/* Interrupted first insert: a key at either ID of a slot without a record is destroyed. */
ZTEST(aliro_ud_persistent_key_recovery, test_boot_destroys_keys_of_empty_slot)
{
	zassert_true(FakePsa::PreloadDurable(CommittedKeyId(0), MakeMaterial(0x20)));
	zassert_true(FakePsa::PreloadDurable(StagedKeyId(0), MakeMaterial(0x21)));

	BootOntoPreloadedState();
	zassert_equal(0u, FakePsa::LiveCount(Kind::Durable));
	zassert_equal(2u, FakePsa::DurableDestroyCount());

	/* The slot is reusable at its committed ID. */
	const Stored stored = Insert(kCredentialA, MakeSub(0x22), 0x23);
	zassert_true(FakePsa::IsLive(CommittedKeyId(0), Kind::Durable));
	ExpectResolves(kCredentialA, MakeSub(0x22), stored);

	ExpectCleanTeardown();
}

/*
 * Interrupted replacement: before the record commit the unreferenced key is
 * the new copy, after it the retired one. Both are destroyed and the
 * committed record keeps its key, whichever of the slot's IDs it uses.
 */
ZTEST(aliro_ud_persistent_key_recovery, test_boot_destroys_uncommitted_or_retired_copy)
{
	for (const bool recordAtStaged : { false, true }) {
		ClearAll();
		const CryptoTypes::KeyId committed = recordAtStaged ? StagedKeyId(0) : CommittedKeyId(0);
		const CryptoTypes::KeyId other = recordAtStaged ? CommittedKeyId(0) : StagedKeyId(0);
		const Stored stored{ 7, MakeMaterial(0x30) };

		FakeStorage::Preload(0, MakeRecord(stored.mRecord, kCredentialA, MakeSub(0x31), committed));
		zassert_true(FakePsa::PreloadDurable(committed, stored.mMaterial));
		zassert_true(FakePsa::PreloadDurable(other, MakeMaterial(0x32)));

		BootOntoPreloadedState();
		zassert_true(FakePsa::IsLive(committed, Kind::Durable), "the committed key must survive");
		zassert_false(FakePsa::IsLive(other, Kind::Durable), "the unreferenced key must be destroyed");
		ExpectResolves(kCredentialA, MakeSub(0x31), stored);

		/* The next replacement reuses the freed ID without collision. */
		const Stored replaced = Insert(kCredentialA, MakeSub(0x31), 0x33);
		zassert_equal(stored.mRecord, replaced.mRecord);
		zassert_true(FakePsa::IsLive(other, Kind::Durable));
		ExpectResolves(kCredentialA, MakeSub(0x31), replaced);

		ExpectCleanTeardown();
	}
}

/* A record whose key is missing cannot be authoritative: it is erased. */
ZTEST(aliro_ud_persistent_key_recovery, test_boot_drops_record_without_key)
{
	FakeStorage::Preload(0, MakeRecord(9, kCredentialA, MakeSub(0x40), CommittedKeyId(0)));

	BootOntoPreloadedState();
	zassert_equal(0u, FakeStorage::PresentCount());
	zassert_equal(0u, FakePsa::DurableDestroyCount());
	ExpectMiss(kCredentialA, MakeSub(0x40));

	const Stored stored = Insert(kCredentialA, MakeSub(0x40), 0x41);
	ExpectResolves(kCredentialA, MakeSub(0x40), stored);

	ExpectCleanTeardown();
}

/*
 * A record naming a key ID outside its slot's pair is erased, and the key
 * it names is left to its owner: another slot's committed key survives.
 */
ZTEST(aliro_ud_persistent_key_recovery, test_boot_drops_record_with_foreign_key_id)
{
	FakeStorage::Preload(0, MakeRecord(3, kCredentialA, MakeSub(0x50), 0x4000FFFF));

	Stored owner{};
	if (kGlobal >= 2) {
		owner = Stored{ 4, MakeMaterial(0x51) };
		FakeStorage::Preload(1, MakeRecord(owner.mRecord, kCredentialB, MakeSub(0x52), CommittedKeyId(1)));
		zassert_true(FakePsa::PreloadDurable(CommittedKeyId(1), owner.mMaterial));
		FakeStorage::Preload(0, MakeRecord(3, kCredentialA, MakeSub(0x50), CommittedKeyId(1)));
	}

	BootOntoPreloadedState();
	ExpectMiss(kCredentialA, MakeSub(0x50));
	zassert_equal(0u, FakePsa::DurableDestroyCount(), "another slot's key must not be destroyed");
	if (kGlobal >= 2) {
		zassert_equal(1u, FakeStorage::PresentCount());
		ExpectResolves(kCredentialB, MakeSub(0x52), owner);
	} else {
		zassert_equal(0u, FakeStorage::PresentCount());
	}

	ExpectCleanTeardown();
}

/* Malformed records are erased and the keys at their slot's IDs destroyed. */
ZTEST(aliro_ud_persistent_key_recovery, test_boot_drops_malformed_records)
{
	enum class Defect { NotValid, InvalidHandle, InvalidCredential };

	for (const Defect defect : { Defect::NotValid, Defect::InvalidHandle, Defect::InvalidCredential }) {
		ClearAll();
		Record record = MakeRecord(5, kCredentialA, MakeSub(0x60), CommittedKeyId(0));
		switch (defect) {
		case Defect::NotValid:
			record.mValid = false;
			break;
		case Defect::InvalidHandle:
			record.mHandle = PK::kInvalidRecordHandle;
			break;
		case Defect::InvalidCredential:
			record.mCredentialHandle = kInvalidCredentialHandle;
			break;
		}
		FakeStorage::Preload(0, record);
		zassert_true(FakePsa::PreloadDurable(CommittedKeyId(0), MakeMaterial(0x61)));

		BootOntoPreloadedState();
		zassert_equal(0u, FakeStorage::PresentCount(), "defect %d: the record must be erased",
			      static_cast<int>(defect));
		zassert_equal(0u, FakePsa::LiveCount(Kind::Durable), "defect %d: its key must be destroyed",
			      static_cast<int>(defect));
		ExpectMiss(kCredentialA, MakeSub(0x60));
	}

	ExpectCleanTeardown();
}

/* Of two records with the same pair or the same handle, the lower slot wins. */
ZTEST(aliro_ud_persistent_key_recovery, test_boot_drops_duplicate_records)
{
	if (kGlobal < 2) {
		ztest_test_skip();
	}

	for (const bool duplicateHandle : { false, true }) {
		ClearAll();
		const Stored kept{ 5, MakeMaterial(0x70) };
		FakeStorage::Preload(0, MakeRecord(kept.mRecord, kCredentialA, MakeSub(0x71), CommittedKeyId(0)));
		zassert_true(FakePsa::PreloadDurable(CommittedKeyId(0), kept.mMaterial));

		const Record duplicate =
			duplicateHandle ? MakeRecord(kept.mRecord, kCredentialB, MakeSub(0x72), CommittedKeyId(1)) :
					  MakeRecord(6, kCredentialA, MakeSub(0x71), CommittedKeyId(1));
		FakeStorage::Preload(1, duplicate);
		zassert_true(FakePsa::PreloadDurable(CommittedKeyId(1), MakeMaterial(0x73)));

		BootOntoPreloadedState();
		zassert_equal(1u, FakeStorage::PresentCount());
		zassert_false(FakePsa::IsLive(CommittedKeyId(1), Kind::Durable), "the duplicate's key is destroyed");
		ExpectResolves(kCredentialA, MakeSub(0x71), kept);
		if (duplicateHandle) {
			ExpectMiss(kCredentialB, MakeSub(0x72));
		}

		ExpectCleanTeardown();
	}
}

/* A recovery step that fails is reported and retried by the next boot. */
ZTEST(aliro_ud_persistent_key_recovery, test_boot_recovery_failure_is_retried)
{
	enum class Failure { Exists, Destroy, Erase };

	for (const Failure failure : { Failure::Exists, Failure::Destroy, Failure::Erase }) {
		ClearAll();
		/* A committed record with an unreferenced key at its other ID... */
		const Stored stored{ 8, MakeMaterial(0x80) };
		const bool keepsCommitted = failure != Failure::Erase || kGlobal >= 2;
		if (keepsCommitted) {
			FakeStorage::Preload(0, MakeRecord(stored.mRecord, kCredentialA, MakeSub(0x81),
							   CommittedKeyId(0)));
		}
		zassert_true(FakePsa::PreloadDurable(CommittedKeyId(0), stored.mMaterial));
		zassert_true(FakePsa::PreloadDurable(StagedKeyId(0), MakeMaterial(0x82)));
		/* ...and, for the erase failure, a record that must be dropped. */
		if (failure == Failure::Erase) {
			FakeStorage::Preload(keepsCommitted ? 1 : 0,
					     MakeRecord(9, kCredentialA, MakeSub(0x83), 0x4000FFFF));
		}

		switch (failure) {
		case Failure::Exists:
			FakePsa::FailNext(FakePsa::Fault::Exists);
			break;
		case Failure::Destroy:
			FakePsa::FailNext(FakePsa::Fault::Destroy);
			break;
		case Failure::Erase:
			FakeStorage::FailNextErase();
			break;
		}
		zassert_not_equal(ALIRO_NO_ERROR, Store::Init(), "failure %d must be reported",
				  static_cast<int>(failure));

		BootOntoPreloadedState();
		if (keepsCommitted) {
			ExpectResolves(kCredentialA, MakeSub(0x81), stored);
		}
		ExpectMiss(kCredentialA, MakeSub(0x83));

		ExpectCleanTeardown();
	}
}

/* Recovery interrupted at every durable step converges on the same state. */
ZTEST(aliro_ud_persistent_key_recovery, test_interrupted_recovery_converges)
{
	const Stored kept{ 11, MakeMaterial(0x90) };
	size_t cut = 0;

	for (;; ++cut) {
		ClearAll();
		FakeStorage::Preload(0, MakeRecord(kept.mRecord, kCredentialA, MakeSub(0x91), StagedKeyId(0)));
		zassert_true(FakePsa::PreloadDurable(StagedKeyId(0), kept.mMaterial));
		zassert_true(FakePsa::PreloadDurable(CommittedKeyId(0), MakeMaterial(0x92)));
		if (kGlobal >= 2) {
			Record malformed = MakeRecord(12, kCredentialB, MakeSub(0x93), CommittedKeyId(1));
			malformed.mValid = false;
			FakeStorage::Preload(1, malformed);
			zassert_true(FakePsa::PreloadDurable(CommittedKeyId(1), MakeMaterial(0x94)));
			zassert_true(FakePsa::PreloadDurable(StagedKeyId(1), MakeMaterial(0x95)));
		}

		FakePower::CutAfter(cut);
		(void)Store::Init();
		const bool tripped = FakePower::Tripped();

		BootOntoPreloadedState();
		zassert_equal(1u, FakeStorage::PresentCount(), "cut %zu", cut);
		ExpectResolves(kCredentialA, MakeSub(0x91), kept);
		ExpectCleanTeardown();

		if (!tripped) {
			break;
		}
	}

	/* One orphan destroy, plus one erase and two destroys with the malformed slot. */
	zassert_equal(kGlobal >= 2 ? 4u : 1u, cut, "every durable recovery step must have been interrupted");
}

/* First insert: a failed Replace() leaves nothing; a successful one survives. */
ZTEST(aliro_ud_persistent_key_recovery, test_power_loss_during_first_insert)
{
	const ReaderGroupSubIdentifier sub = MakeSub(0xA0);
	size_t cut = 0;

	for (;; ++cut) {
		ColdBoot();
		const FakePsa::Material material = MakeMaterial(0xA1);
		const CryptoTypes::KeyId input = FakePsa::CreateInputKey(material);
		PK::RecordHandle record{};

		FakePower::CutAfter(cut);
		const AliroError error = PK::Replace(kCredentialA, sub, input, record);
		const bool tripped = FakePower::Tripped();

		zassert_equal(ALIRO_NO_ERROR, Reboot());
		ExpectConsistent();
		if (error == ALIRO_NO_ERROR) {
			ExpectResolves(kCredentialA, sub, Stored{ record, material });
		} else {
			ExpectMiss(kCredentialA, sub);
		}
		ExpectStableReboot();
		ExpectCleanTeardown();

		if (!tripped) {
			zassert_equal(ALIRO_NO_ERROR, error);
			break;
		}
	}

	zassert_equal(2u, cut, "key ownership and record commit must both have been interrupted");
}

/*
 * Same-pair replacement: a failed Replace() preserves the previous record;
 * a successful one exposes only the new record, even if power is lost
 * before the old key is retired.
 */
ZTEST(aliro_ud_persistent_key_recovery, test_power_loss_during_replacement)
{
	const ReaderGroupSubIdentifier sub = MakeSub(0xB0);
	size_t cut = 0;

	for (;; ++cut) {
		ColdBoot();
		const Stored previous = Insert(kCredentialA, sub, 0xB1);
		const FakePsa::Material fresh = MakeMaterial(0xB2);
		const CryptoTypes::KeyId input = FakePsa::CreateInputKey(fresh);
		PK::RecordHandle record{};

		FakePower::CutAfter(cut);
		const AliroError error = PK::Replace(kCredentialA, sub, input, record);
		const bool tripped = FakePower::Tripped();

		zassert_equal(ALIRO_NO_ERROR, Reboot());
		ExpectConsistent();
		zassert_equal(1u, FakeStorage::PresentCount());
		ExpectResolves(kCredentialA, sub,
			       error == ALIRO_NO_ERROR ? Stored{ previous.mRecord, fresh } : previous);
		ExpectStableReboot();
		ExpectCleanTeardown();

		if (!tripped) {
			zassert_equal(ALIRO_NO_ERROR, error);
			break;
		}
	}

	zassert_equal(3u, cut, "ownership, commit, and retirement must all have been interrupted");
}

/* Single-record deletion: the record either survives intact or is gone with its key. */
ZTEST(aliro_ud_persistent_key_recovery, test_power_loss_during_delete)
{
	const ReaderGroupSubIdentifier sub = MakeSub(0xC0);
	size_t cut = 0;

	for (;; ++cut) {
		ColdBoot();
		const Stored stored = Insert(kCredentialA, sub, 0xC1);

		FakePower::CutAfter(cut);
		const AliroError error = PK::Delete(stored.mRecord);
		const bool tripped = FakePower::Tripped();

		zassert_equal(ALIRO_NO_ERROR, Reboot());
		ExpectConsistent();
		const bool survived = ResolvesOrMisses(kCredentialA, sub, stored);
		zassert_false(error == ALIRO_NO_ERROR && survived, "a successful Delete must not be undone");
		ExpectStableReboot();
		ExpectCleanTeardown();

		if (!tripped) {
			zassert_equal(ALIRO_NO_ERROR, error);
			break;
		}
	}

	zassert_equal(2u, cut, "the record erase and the key destroy must both have been interrupted");
}

/* Credential-wide deletion: each record survives intact or is gone; other credentials are untouched. */
ZTEST(aliro_ud_persistent_key_recovery, test_power_loss_during_delete_all_for_credential)
{
	const size_t ownCount = std::min<size_t>(kPerCredential, 2);
	const bool hasOther = kCredentials >= 2;
	size_t cut = 0;

	for (;; ++cut) {
		ColdBoot();
		std::array<Stored, 2> own{};
		for (size_t i = 0; i < ownCount; ++i) {
			own[i] = Insert(kCredentialA, MakeSub(static_cast<uint8_t>(0xD0 + i)),
					static_cast<uint8_t>(0xD4 + i));
		}
		Stored other{};
		if (hasOther) {
			other = Insert(kCredentialB, MakeSub(0xD8), 0xD9);
		}

		FakePower::CutAfter(cut);
		const AliroError error = Store::DeleteAllForCredential(kCredentialA);
		const bool tripped = FakePower::Tripped();

		zassert_equal(ALIRO_NO_ERROR, Reboot());
		ExpectConsistent();
		for (size_t i = 0; i < ownCount; ++i) {
			const bool survived =
				ResolvesOrMisses(kCredentialA, MakeSub(static_cast<uint8_t>(0xD0 + i)), own[i]);
			zassert_false(error == ALIRO_NO_ERROR && survived);
		}
		if (hasOther) {
			ExpectResolves(kCredentialB, MakeSub(0xD8), other);
		}
		ExpectStableReboot();
		ExpectCleanTeardown();

		if (!tripped) {
			zassert_equal(ALIRO_NO_ERROR, error);
			break;
		}
	}

	zassert_equal(2 * ownCount, cut);
}

/* Factory reset: each record survives intact or is gone with its key. */
ZTEST(aliro_ud_persistent_key_recovery, test_power_loss_during_factory_reset)
{
	const size_t count = std::min<size_t>(kGlobal, 3);
	size_t cut = 0;

	for (;; ++cut) {
		ColdBoot();
		std::array<Stored, 3> stored{};
		for (size_t i = 0; i < count; ++i) {
			stored[i] = Insert(CredentialAt(i), MakeSub(static_cast<uint8_t>(0xE0 + i)),
					   static_cast<uint8_t>(0xE4 + i));
		}

		FakePower::CutAfter(cut);
		const AliroError error = PK::Reset();
		const bool tripped = FakePower::Tripped();

		zassert_equal(ALIRO_NO_ERROR, Reboot());
		ExpectConsistent();
		for (size_t i = 0; i < count; ++i) {
			const bool survived =
				ResolvesOrMisses(CredentialAt(i), MakeSub(static_cast<uint8_t>(0xE0 + i)), stored[i]);
			zassert_false(error == ALIRO_NO_ERROR && survived);
		}
		ExpectStableReboot();
		ExpectCleanTeardown();

		if (!tripped) {
			zassert_equal(ALIRO_NO_ERROR, error);
			break;
		}
	}

	zassert_equal(2 * count, cut);
}

/* A failed record erase is returned and leaves the record and its key untouched. */
ZTEST(aliro_ud_persistent_key_recovery, test_delete_erase_failure_keeps_record)
{
	const Stored stored = Insert(kCredentialA, MakeSub(0x15), 0x16);
	const FakeStorage::Image image = FakeStorage::Capture();

	FakeStorage::FailNextErase();
	zassert_not_equal(ALIRO_NO_ERROR, PK::Delete(stored.mRecord), "the erase failure must be returned");
	zassert_true(FakeStorage::Capture() == image);
	zassert_equal(1u, FakePsa::LiveCount(Kind::Durable));
	ExpectResolves(kCredentialA, MakeSub(0x15), stored);
	ExpectConsistent();

	zassert_equal(ALIRO_NO_ERROR, PK::Delete(stored.mRecord), "a retry must succeed");
	ExpectMiss(kCredentialA, MakeSub(0x15));
	ExpectCleanTeardown();
}

/*
 * A failed key destroy after the erase is returned; the unreferenced key is
 * swept by the next Replace() into the slot, or by the next boot.
 */
ZTEST(aliro_ud_persistent_key_recovery, test_delete_destroy_failure_is_returned_and_swept)
{
	for (const bool sweepAtBoot : { false, true }) {
		ColdBoot();
		const ReaderGroupSubIdentifier sub = MakeSub(0x17);
		const Stored stored = Insert(kCredentialA, sub, 0x18);

		FakePsa::FailNext(FakePsa::Fault::Destroy);
		zassert_not_equal(ALIRO_NO_ERROR, PK::Delete(stored.mRecord), "the destroy failure must be returned");
		ExpectMiss(kCredentialA, sub);
		zassert_equal(0u, FakeStorage::PresentCount());
		zassert_equal(1u, FakePsa::LiveCount(Kind::Durable), "the key is retained for recovery");

		if (sweepAtBoot) {
			zassert_equal(ALIRO_NO_ERROR, Reboot());
			zassert_equal(0u, FakePsa::LiveCount(Kind::Durable));
		} else {
			const Stored reinserted = Insert(kCredentialA, sub, 0x19);
			ExpectResolves(kCredentialA, sub, reinserted);
		}
		ExpectConsistent();
		ExpectStableReboot();
		ExpectCleanTeardown();
	}
}

/* Once committed, Replace() succeeds even if the old key cannot be retired. */
ZTEST(aliro_ud_persistent_key_recovery, test_replace_retire_failure_still_commits)
{
	const ReaderGroupSubIdentifier sub = MakeSub(0x1A);
	(void)Insert(kCredentialA, sub, 0x1B);

	FakePsa::FailNext(FakePsa::Fault::Destroy);
	const Stored fresh = Insert(kCredentialA, sub, 0x1C);
	ExpectResolves(kCredentialA, sub, fresh);
	zassert_equal(2u, FakePsa::LiveCount(Kind::Durable), "the old key is retained for recovery");

	/* The next replacement sweeps it before reusing its ID. */
	const Stored next = Insert(kCredentialA, sub, 0x1D);
	ExpectResolves(kCredentialA, sub, next);
	zassert_equal(1u, FakePsa::LiveCount(Kind::Durable));
	ExpectConsistent();

	/* The same failure is also swept at boot. */
	FakePsa::FailNext(FakePsa::Fault::Destroy);
	const Stored last = Insert(kCredentialA, sub, 0x1E);
	zassert_equal(ALIRO_NO_ERROR, Reboot());
	ExpectResolves(kCredentialA, sub, last);
	ExpectConsistent();
	ExpectStableReboot();

	ExpectCleanTeardown();
}

/* If the pre-ownership sweep fails, Replace() fails without touching committed state. */
ZTEST(aliro_ud_persistent_key_recovery, test_replace_sweep_failure_preserves_committed_state)
{
	const ReaderGroupSubIdentifier sub = MakeSub(0x1F);
	(void)Insert(kCredentialA, sub, 0x20);
	FakePsa::FailNext(FakePsa::Fault::Destroy);
	const Stored committed = Insert(kCredentialA, sub, 0x21);
	zassert_equal(2u, FakePsa::LiveCount(Kind::Durable));

	FakePsa::FailNext(FakePsa::Fault::Exists);
	ExpectReplaceRejected(kCredentialA, sub);
	ExpectResolves(kCredentialA, sub, committed);

	FakePsa::FailNext(FakePsa::Fault::Destroy);
	ExpectReplaceRejected(kCredentialA, sub);
	ExpectResolves(kCredentialA, sub, committed);

	const Stored next = Insert(kCredentialA, sub, 0x22);
	ExpectResolves(kCredentialA, sub, next);
	ExpectConsistent();
	ExpectCleanTeardown();
}

/* Bulk removal continues past a failure, returns it, and a retry completes. */
ZTEST(aliro_ud_persistent_key_recovery, test_bulk_removal_continues_past_failures)
{
	if (kPerCredential >= 2) {
		const Stored first = Insert(kCredentialA, MakeSub(0x23), 0x24);
		const Stored second = Insert(kCredentialA, MakeSub(0x25), 0x26);

		FakeStorage::FailNextErase();
		zassert_not_equal(ALIRO_NO_ERROR, Store::DeleteAllForCredential(kCredentialA));
		ExpectResolves(kCredentialA, MakeSub(0x23), first);
		ExpectMiss(kCredentialA, MakeSub(0x25));
		(void)second;
		ExpectConsistent();

		zassert_equal(ALIRO_NO_ERROR, Store::DeleteAllForCredential(kCredentialA));
		ExpectMiss(kCredentialA, MakeSub(0x23));
		ExpectConsistent();
	}

	const size_t count = std::min<size_t>(kGlobal, 2);
	for (size_t i = 0; i < count; ++i) {
		(void)Insert(CredentialAt(i), MakeSub(static_cast<uint8_t>(0x27 + i)), static_cast<uint8_t>(0x29 + i));
	}

	FakePsa::FailNext(FakePsa::Fault::Destroy);
	zassert_not_equal(ALIRO_NO_ERROR, PK::Reset(), "the destroy failure must be returned");
	zassert_equal(0u, FakeStorage::PresentCount(), "every record is still erased");
	zassert_equal(1u, FakePsa::LiveCount(Kind::Durable), "the failed key is retained for recovery");

	/* A repeated factory reset sweeps it. */
	ExpectCleanTeardown();
}
