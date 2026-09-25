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
#include <cstdint>

/*
 * Fault matrix of the application Kpersistent store. Kpersistent is a
 * long-term key kept in non-volatile memory (Aliro 1.0 Specification,
 * section 3.1, page 23; section 8.3.1.13, page 59), and its lifecycle
 * management is implementation-specific (section 6.2, page 28).
 *
 * For every operation, the clean run fixes the exact sequence of durable
 * transitions. Each transition is then failed once without its effect
 * ("before") and once after it ("after"), power is cut before each of them,
 * and each presence query is failed. Every row checks the returned error,
 * the visible record, and the surviving key objects, then boots twice and
 * checks the recovered state and that the second boot changes nothing.
 */

using namespace Aliro;
using namespace Aliro::UserDevice;
using namespace AliroUd::PersistentKey::RecoverySupport;

namespace Store = ::AliroUd::PersistentKey::Store;
namespace FakePsa = ::AliroUd::PersistentKey::FakePsa;
namespace FakeStorage = ::AliroUd::PersistentKey::FakeStorage;
namespace FakePower = ::AliroUd::PersistentKey::FakePower;

using AliroUd::PersistentKey::Record;
using FakePower::Mutation;
using FakePower::Transition;
using FakePsa::Kind;

#define ROW_FMT "%s/%s/%zu: "
#define ROW_ARGS(id) (id).mOperation, (id).mFault, (id).mIndex

namespace {

constexpr size_t kPerCredential{ AliroUd::PersistentKey::kMaxRecordsPerCredential };
constexpr size_t kGlobal{ AliroUd::PersistentKey::kMaxRecords };
constexpr size_t kCredentials{ CONFIG_ALIRO_UD_MAX_CREDENTIALS };

constexpr CredentialHandle kCredentialA{ 1 };
constexpr CredentialHandle kCredentialB{ 2 };

enum class State : uint8_t {
	Prior,
	New,
};

constexpr uint8_t kNoKey{ 0 };
constexpr uint8_t kOldKey{ 1 };
constexpr uint8_t kNewKey{ 2 };

/* Expected outcome of one injected failure. */
struct Expect {
	bool mError;
	State mVisible;
	uint8_t mKeys;
	State mRecovered;
};

struct RowId {
	const char *mOperation;
	const char *mFault;
	size_t mIndex;
};

struct Transitions {
	std::array<Transition, 8> mItems{};
	size_t mCount{ 0 };

	void Add(Transition transition)
	{
		zassert_true(mCount < mItems.size());
		mItems[mCount++] = transition;
	}
};

Transition Own(CryptoTypes::KeyId keyId)
{
	return { Mutation::OwnKey, keyId };
}

Transition Destroy(CryptoTypes::KeyId keyId)
{
	return { Mutation::DestroyKey, keyId };
}

Transition Save(size_t slotIndex)
{
	return { Mutation::SaveRecord, static_cast<uint32_t>(slotIndex) };
}

Transition Erase(size_t slotIndex)
{
	return { Mutation::EraseRecord, static_cast<uint32_t>(slotIndex) };
}

const char *FaultName(bool withEffect)
{
	return withEffect ? "fail-after" : "fail-before";
}

CredentialHandle CredentialAt(size_t index)
{
	return static_cast<CredentialHandle>(index % kCredentials + 1);
}

void ColdBoot()
{
	ClearAll();
	zassert_equal(ALIRO_NO_ERROR, Store::Init(), "store init must succeed on empty storage");
}

void ExpectTrace(const RowId &id, const Transitions &expected)
{
	zassert_true(FakePower::TraceLength() <= FakePower::TraceCapacity());
	zassert_equal(expected.mCount, FakePower::TraceLength(), ROW_FMT "unexpected number of durable transitions",
		      ROW_ARGS(id));
	for (size_t i = 0; i < expected.mCount; ++i) {
		zassert_true(FakePower::TraceAt(i) == expected.mItems[i], ROW_FMT "transition %zu differs",
			     ROW_ARGS(id), i);
	}
}

void ExpectFaultAt(const RowId &id, const Transitions &transitions, size_t index)
{
	zassert_true(FakePower::Tripped(), ROW_FMT "the fault must fire", ROW_ARGS(id));
	zassert_true(FakePower::TraceAt(index) == transitions.mItems[index],
		     ROW_FMT "the fault must hit the enumerated transition", ROW_ARGS(id));
}

void ExpectNoStrayObjects(const RowId &id)
{
	zassert_equal(0u, FakePsa::LiveCount(Kind::Input), ROW_FMT "no input key may survive", ROW_ARGS(id));
	zassert_equal(0u, FakePsa::LiveCount(Kind::Temporary), ROW_FMT "no temporary key may survive", ROW_ARGS(id));
	zassert_equal(0u, FakePsa::InvalidDestroyCount(), ROW_FMT "no key may be destroyed twice or while absent",
		      ROW_ARGS(id));
}

/*
 * Returns true on a hit, which must wrap `material` and, unless `handle` is
 * invalid, carry it; returns false on a miss.
 */
bool Resolves(const RowId &id, CredentialHandle credential, const ReaderGroupSubIdentifier &sub,
	      const FakePsa::Material &material, PK::RecordHandle handle)
{
	PK::RecordHandle record{ PK::kInvalidRecordHandle };
	CryptoTypes::KeyId temporary{ 0 };

	const AliroError error = PK::Lookup(credential, sub, record, temporary);
	if (error == ALIRO_ERROR_UNKNOWN) {
		zassert_equal(PK::kInvalidRecordHandle, record);
		zassert_equal(0u, temporary);
		return false;
	}

	zassert_equal(ALIRO_NO_ERROR, error, ROW_FMT "Lookup must either hit or miss", ROW_ARGS(id));
	zassert_not_equal(PK::kInvalidRecordHandle, record);
	if (handle != PK::kInvalidRecordHandle) {
		zassert_equal(handle, record, ROW_FMT "Lookup must return the committed handle", ROW_ARGS(id));
	}
	zassert_true(FakePsa::MaterialEquals(temporary, material), ROW_FMT "Lookup must wrap the expected Kpersistent",
		     ROW_ARGS(id));
	zassert_true(FakePsa::DestroyCallerKey(temporary));
	return true;
}

/* ---- Insert, replacement, and single delete of one record in slot 0 ---- */

enum class Op : uint8_t {
	Insert,
	Replace,
	Delete,
};

struct SlotCase {
	Op mOp{ Op::Insert };
	Stored mPrior{};
	CryptoTypes::KeyId mOldKeyId{ 0 };
	CryptoTypes::KeyId mNewKeyId{ 0 };
	FakePsa::Material mNewMaterial{};
	PK::RecordHandle mNewRecord{ PK::kInvalidRecordHandle };
};

ReaderGroupSubIdentifier SlotSub()
{
	return MakeSub(0x31);
}

SlotCase PrepareSlotCase(Op op, bool priorAtStaged)
{
	ColdBoot();

	SlotCase slotCase{};
	slotCase.mOp = op;
	slotCase.mNewMaterial = MakeMaterial(0x33);
	if (op == Op::Insert) {
		slotCase.mNewKeyId = CommittedKeyId(0);
	} else {
		slotCase.mPrior = Insert(kCredentialA, SlotSub(), 0x31);
		if (priorAtStaged) {
			slotCase.mPrior = Insert(kCredentialA, SlotSub(), 0x32);
		}
		slotCase.mOldKeyId = priorAtStaged ? StagedKeyId(0) : CommittedKeyId(0);
		if (op == Op::Replace) {
			slotCase.mNewKeyId = priorAtStaged ? CommittedKeyId(0) : StagedKeyId(0);
		}
	}

	FakePower::ClearTrace();
	return slotCase;
}

AliroError RunSlotOperation(SlotCase &slotCase)
{
	if (slotCase.mOp == Op::Delete) {
		return PK::Delete(slotCase.mPrior.mRecord);
	}

	const CryptoTypes::KeyId input = FakePsa::CreateInputKey(slotCase.mNewMaterial);
	zassert_not_equal(0u, input);
	const AliroError error = PK::Replace(kCredentialA, SlotSub(), input, slotCase.mNewRecord);
	/* The stack releases its input key after every Replace(). */
	zassert_true(FakePsa::DestroyCallerKey(input));
	return error;
}

void ExpectSlotState(const RowId &id, const SlotCase &slotCase, State state)
{
	const bool isNew = state == State::New;
	const bool hit = slotCase.mOp == Op::Insert ? isNew : (slotCase.mOp == Op::Replace || !isNew);
	const bool newMaterial = isNew && slotCase.mOp != Op::Delete;
	const PK::RecordHandle handle = slotCase.mOp == Op::Insert ? slotCase.mNewRecord : slotCase.mPrior.mRecord;

	zassert_equal(hit,
		      Resolves(id, kCredentialA, SlotSub(),
			       newMaterial ? slotCase.mNewMaterial : slotCase.mPrior.mMaterial, handle),
		      ROW_FMT "the pair must %s", ROW_ARGS(id), hit ? "resolve" : "miss");
}

void ExpectSlotKeys(const RowId &id, const SlotCase &slotCase, uint8_t keys)
{
	size_t expected{ 0 };
	if ((keys & kOldKey) != 0) {
		zassert_true(FakePsa::IsLive(slotCase.mOldKeyId, Kind::Durable), ROW_FMT "the old key must survive",
			     ROW_ARGS(id));
		++expected;
	}
	if ((keys & kNewKey) != 0) {
		zassert_true(FakePsa::IsLive(slotCase.mNewKeyId, Kind::Durable), ROW_FMT "the new key must survive",
			     ROW_ARGS(id));
		++expected;
	}
	zassert_equal(expected, FakePsa::LiveCount(Kind::Durable), ROW_FMT "no unexplained durable key may exist",
		      ROW_ARGS(id));
	ExpectNoStrayObjects(id);
}

uint8_t RecoveredKeys(const SlotCase &slotCase, State state)
{
	if (state == State::Prior) {
		return slotCase.mOp == Op::Insert ? kNoKey : kOldKey;
	}
	return slotCase.mOp == Op::Delete ? kNoKey : kNewKey;
}

void ExpectSlotRow(const RowId &id, const SlotCase &slotCase, AliroError error, const Expect &expect)
{
	zassert_equal(expect.mError, error != ALIRO_NO_ERROR, ROW_FMT "unexpected result %d", ROW_ARGS(id),
		      error.ToInt());
	ExpectSlotState(id, slotCase, expect.mVisible);
	ExpectSlotKeys(id, slotCase, expect.mKeys);
}

/* Boots twice: the first boot recovers `recovered`, the second changes nothing. */
void FinishSlotRow(const RowId &id, const SlotCase &slotCase, State recovered)
{
	zassert_equal(ALIRO_NO_ERROR, Reboot(), ROW_FMT "recovery must succeed", ROW_ARGS(id));
	ExpectSlotState(id, slotCase, recovered);
	ExpectSlotKeys(id, slotCase, RecoveredKeys(slotCase, recovered));
	ExpectConsistent();

	ExpectStableReboot();
	ExpectSlotState(id, slotCase, recovered);
	ExpectCleanTeardown();
}

struct SlotMatrix {
	const char *mName;
	Op mOp;
	bool mPriorAtStaged;
	Transitions mTransitions;
	const Expect *mInjected; /* before, after for each transition */
	const State *mPowerCut;  /* recovered state after each cut, then after completion */
	const Expect *mPresence; /* one per presence query */
	size_t mPresenceCount;
};

void RunSlotMatrix(const SlotMatrix &matrix)
{
	const size_t count = matrix.mTransitions.mCount;

	{
		const RowId id{ matrix.mName, "none", 0 };
		SlotCase slotCase = PrepareSlotCase(matrix.mOp, matrix.mPriorAtStaged);
		zassert_equal(ALIRO_NO_ERROR, RunSlotOperation(slotCase));
		ExpectTrace(id, matrix.mTransitions);
		FinishSlotRow(id, slotCase, State::New);
	}

	for (size_t k = 0; k < count; ++k) {
		for (const bool withEffect : { false, true }) {
			const RowId id{ matrix.mName, FaultName(withEffect), k };
			const Expect &expect = matrix.mInjected[2 * k + (withEffect ? 1 : 0)];
			SlotCase slotCase = PrepareSlotCase(matrix.mOp, matrix.mPriorAtStaged);

			FakePower::FailOnceAfter(k, withEffect);
			const AliroError error = RunSlotOperation(slotCase);
			ExpectFaultAt(id, matrix.mTransitions, k);
			ExpectSlotRow(id, slotCase, error, expect);
			FinishSlotRow(id, slotCase, expect.mRecovered);
		}
	}

	for (size_t n = 0; n <= count; ++n) {
		const RowId id{ matrix.mName, "power-cut", n };
		SlotCase slotCase = PrepareSlotCase(matrix.mOp, matrix.mPriorAtStaged);

		FakePower::CutAfter(n);
		(void)RunSlotOperation(slotCase);
		zassert_equal(n < count, FakePower::Tripped(), ROW_FMT "the cut must interrupt the operation",
			      ROW_ARGS(id));
		FinishSlotRow(id, slotCase, matrix.mPowerCut[n]);
	}

	for (size_t q = 0;; ++q) {
		const RowId id{ matrix.mName, "presence", q };
		SlotCase slotCase = PrepareSlotCase(matrix.mOp, matrix.mPriorAtStaged);

		FakePsa::FailNext(FakePsa::Fault::Exists, q);
		const AliroError error = RunSlotOperation(slotCase);
		if (FakePsa::FaultArmed()) {
			FakePsa::FailNext(FakePsa::Fault::None);
			zassert_equal(ALIRO_NO_ERROR, error);
			zassert_equal(matrix.mPresenceCount, q, ROW_FMT "unexpected number of presence queries",
				      ROW_ARGS(id));
			ExpectCleanTeardown();
			break;
		}
		zassert_true(q < matrix.mPresenceCount, ROW_FMT "unexpected presence query", ROW_ARGS(id));
		ExpectSlotRow(id, slotCase, error, matrix.mPresence[q]);
		FinishSlotRow(id, slotCase, matrix.mPresence[q].mRecovered);
	}
}

/* Own(new), Save(record). */
constexpr std::array<Expect, 4> kInsertRows{ {
	{ true, State::Prior, kNoKey, State::Prior },
	{ true, State::Prior, kNewKey, State::Prior },
	{ true, State::Prior, kNoKey, State::Prior },
	{ false, State::New, kNewKey, State::New },
} };
constexpr std::array<State, 3> kInsertPower{ { State::Prior, State::Prior, State::New } };
constexpr std::array<Expect, 2> kInsertPresence{ {
	{ true, State::Prior, kNoKey, State::Prior },
	{ true, State::Prior, kNoKey, State::Prior },
} };

/* Own(new), Save(record), Destroy(old). */
constexpr std::array<Expect, 6> kReplaceRows{ {
	{ true, State::Prior, kOldKey, State::Prior },
	{ true, State::Prior, kOldKey | kNewKey, State::Prior },
	{ true, State::Prior, kOldKey, State::Prior },
	{ false, State::New, kNewKey, State::New },
	{ false, State::New, kOldKey | kNewKey, State::New },
	{ false, State::New, kNewKey, State::New },
} };
constexpr std::array<State, 4> kReplacePower{ { State::Prior, State::Prior, State::New, State::New } };
constexpr std::array<Expect, 1> kReplacePresence{ {
	{ true, State::Prior, kOldKey, State::Prior },
} };

/* Erase(record), Destroy(old). */
constexpr std::array<Expect, 4> kDeleteRows{ {
	{ true, State::Prior, kOldKey, State::Prior },
	{ true, State::Prior, kOldKey, State::New },
	{ true, State::New, kOldKey, State::New },
	{ true, State::New, kNoKey, State::New },
} };
constexpr std::array<State, 3> kDeletePower{ { State::Prior, State::New, State::New } };
/* The sweep queries the committed ID first, then the staged ID. */
constexpr std::array<Expect, 2> kDeletePresenceAtCommitted{ {
	{ true, State::New, kOldKey, State::New },
	{ true, State::New, kNoKey, State::New },
} };
constexpr std::array<Expect, 2> kDeletePresenceAtStaged{ {
	{ true, State::New, kNoKey, State::New },
	{ true, State::New, kOldKey, State::New },
} };

/* ---- Credential-wide delete and factory reset over several slots ---- */

enum class Bulk : uint8_t {
	CredentialWide,
	FactoryReset,
};

constexpr size_t kMaxBulk{ 3 };

struct BulkCase {
	Bulk mKind{ Bulk::CredentialWide };
	size_t mRemoved{ 0 };
	bool mHasOther{ false };
	std::array<CredentialHandle, kMaxBulk> mCredential{};
	std::array<Stored, kMaxBulk> mStored{};
};

/* Per removed record: the state and whether its key object survives. */
struct BulkExpect {
	std::array<State, kMaxBulk> mState{};
	std::array<bool, kMaxBulk> mKey{};
};

ReaderGroupSubIdentifier BulkSub(size_t index)
{
	return MakeSub(static_cast<uint8_t>(0x50 + index));
}

BulkCase PrepareBulkCase(Bulk kind)
{
	ColdBoot();

	BulkCase bulk{};
	bulk.mKind = kind;
	if (kind == Bulk::CredentialWide) {
		bulk.mRemoved = std::min<size_t>(kPerCredential, 2);
		bulk.mHasOther = kCredentials >= 2;
	} else {
		bulk.mRemoved = std::min<size_t>(kGlobal, kMaxBulk);
	}

	const size_t total = bulk.mRemoved + (bulk.mHasOther ? 1 : 0);
	for (size_t i = 0; i < total; ++i) {
		const bool other = i == bulk.mRemoved;
		bulk.mCredential[i] = kind == Bulk::FactoryReset ? CredentialAt(i) :
				      other			 ? kCredentialB :
								   kCredentialA;
		bulk.mStored[i] = Insert(bulk.mCredential[i], BulkSub(i), static_cast<uint8_t>(0x58 + i));
	}

	FakePower::ClearTrace();
	return bulk;
}

AliroError RunBulkOperation(const BulkCase &bulk)
{
	return bulk.mKind == Bulk::CredentialWide ? Store::DeleteAllForCredential(kCredentialA) : PK::Reset();
}

Transitions BulkTransitions(const BulkCase &bulk)
{
	Transitions transitions{};
	for (size_t i = 0; i < bulk.mRemoved; ++i) {
		transitions.Add(Erase(i));
		transitions.Add(Destroy(CommittedKeyId(i)));
	}
	return transitions;
}

BulkExpect AllRemoved()
{
	BulkExpect expect{};
	expect.mState.fill(State::New);
	return expect;
}

void ExpectBulkState(const RowId &id, const BulkCase &bulk, const BulkExpect &expect)
{
	size_t durable{ 0 };

	for (size_t i = 0; i < bulk.mRemoved; ++i) {
		const bool hit = expect.mState[i] == State::Prior;
		zassert_equal(hit,
			      Resolves(id, bulk.mCredential[i], BulkSub(i), bulk.mStored[i].mMaterial,
				       bulk.mStored[i].mRecord),
			      ROW_FMT "record %zu must %s", ROW_ARGS(id), i, hit ? "resolve" : "miss");
		zassert_equal(expect.mKey[i], FakePsa::IsLive(CommittedKeyId(i), Kind::Durable),
			      ROW_FMT "unexpected key state of record %zu", ROW_ARGS(id), i);
		durable += expect.mKey[i] ? 1 : 0;
	}

	if (bulk.mHasOther) {
		const size_t i = bulk.mRemoved;
		zassert_true(Resolves(id, bulk.mCredential[i], BulkSub(i), bulk.mStored[i].mMaterial,
				      bulk.mStored[i].mRecord),
			     ROW_FMT "another credential's record must be untouched", ROW_ARGS(id));
		zassert_true(FakePsa::IsLive(CommittedKeyId(i), Kind::Durable));
		++durable;
	}

	zassert_equal(durable, FakePsa::LiveCount(Kind::Durable), ROW_FMT "no unexplained durable key may exist",
		      ROW_ARGS(id));
	ExpectNoStrayObjects(id);
}

void FinishBulkRow(const RowId &id, const BulkCase &bulk, BulkExpect recovered)
{
	for (size_t i = 0; i < bulk.mRemoved; ++i) {
		recovered.mKey[i] = recovered.mState[i] == State::Prior;
	}

	zassert_equal(ALIRO_NO_ERROR, Reboot(), ROW_FMT "recovery must succeed", ROW_ARGS(id));
	ExpectBulkState(id, bulk, recovered);
	ExpectConsistent();

	ExpectStableReboot();
	ExpectBulkState(id, bulk, recovered);
	ExpectCleanTeardown();
}

/*
 * Removal visits records in slot order and continues past failures, so the
 * faulted record behaves as a single delete and every other one is removed.
 */
void RunBulkMatrix(Bulk kind, const char *name)
{
	const BulkCase shape = PrepareBulkCase(kind);
	const Transitions transitions = BulkTransitions(shape);
	ExpectCleanTeardown();

	{
		const RowId id{ name, "none", 0 };
		const BulkCase bulk = PrepareBulkCase(kind);
		zassert_equal(ALIRO_NO_ERROR, RunBulkOperation(bulk));
		ExpectTrace(id, transitions);
		FinishBulkRow(id, bulk, AllRemoved());
	}

	for (size_t k = 0; k < transitions.mCount; ++k) {
		for (const bool withEffect : { false, true }) {
			const RowId id{ name, FaultName(withEffect), k };
			const size_t faulted = k / 2;
			const Expect &row = kDeleteRows[2 * (k % 2) + (withEffect ? 1 : 0)];
			const BulkCase bulk = PrepareBulkCase(kind);

			FakePower::FailOnceAfter(k, withEffect);
			const AliroError error = RunBulkOperation(bulk);
			ExpectFaultAt(id, transitions, k);
			zassert_equal(row.mError, error != ALIRO_NO_ERROR, ROW_FMT "unexpected result", ROW_ARGS(id));

			BulkExpect visible = AllRemoved();
			visible.mState[faulted] = row.mVisible;
			visible.mKey[faulted] = (row.mKeys & kOldKey) != 0;
			ExpectBulkState(id, bulk, visible);

			BulkExpect recovered = AllRemoved();
			recovered.mState[faulted] = row.mRecovered;
			FinishBulkRow(id, bulk, recovered);
		}
	}

	for (size_t n = 0; n <= transitions.mCount; ++n) {
		const RowId id{ name, "power-cut", n };
		const BulkCase bulk = PrepareBulkCase(kind);

		FakePower::CutAfter(n);
		(void)RunBulkOperation(bulk);
		zassert_equal(n < transitions.mCount, FakePower::Tripped());

		/* A record is gone once its erase (transition 2i) has landed. */
		BulkExpect recovered = AllRemoved();
		for (size_t i = 0; i < bulk.mRemoved; ++i) {
			recovered.mState[i] = n > 2 * i ? State::New : State::Prior;
		}
		FinishBulkRow(id, bulk, recovered);
	}

	/* Each visited slot queries its committed ID, then its staged ID. */
	const size_t visited = kind == Bulk::CredentialWide ? shape.mRemoved : kGlobal;
	for (size_t q = 0;; ++q) {
		const RowId id{ name, "presence", q };
		const BulkCase bulk = PrepareBulkCase(kind);

		FakePsa::FailNext(FakePsa::Fault::Exists, q);
		const AliroError error = RunBulkOperation(bulk);
		if (FakePsa::FaultArmed()) {
			FakePsa::FailNext(FakePsa::Fault::None);
			zassert_equal(ALIRO_NO_ERROR, error);
			zassert_equal(2 * visited, q, ROW_FMT "unexpected number of presence queries", ROW_ARGS(id));
			ExpectCleanTeardown();
			break;
		}
		zassert_not_equal(ALIRO_NO_ERROR, error, ROW_FMT "the failure must be returned", ROW_ARGS(id));

		BulkExpect visible = AllRemoved();
		const size_t slot = q / 2;
		if (q % 2 == 0 && slot < bulk.mRemoved) {
			visible.mKey[slot] = true;
		}
		ExpectBulkState(id, bulk, visible);
		FinishBulkRow(id, bulk, AllRemoved());
	}
}

/* ---- Boot recovery over every kind of recoverable state ---- */

struct BootCase {
	Stored mKept{};
	bool mHasMalformed{ false };
	bool mHasKeyless{ false };
};

ReaderGroupSubIdentifier KeptSub()
{
	return MakeSub(0x61);
}

/*
 * Slot 0: a committed record at its staged ID plus the uncommitted or
 * retired copy at its committed ID. Slot 1: a malformed record with keys at
 * both IDs. Slot 2: a record whose key is missing.
 */
BootCase PrepareBootCase()
{
	ClearAll();

	BootCase boot{};
	boot.mKept = Stored{ 11, MakeMaterial(0x60) };
	FakeStorage::Preload(0, MakeRecord(boot.mKept.mRecord, kCredentialA, KeptSub(), StagedKeyId(0)));
	zassert_true(FakePsa::PreloadDurable(StagedKeyId(0), boot.mKept.mMaterial));
	zassert_true(FakePsa::PreloadDurable(CommittedKeyId(0), MakeMaterial(0x62)));

	boot.mHasMalformed = kGlobal >= 2;
	if (boot.mHasMalformed) {
		Record malformed = MakeRecord(12, kCredentialB, MakeSub(0x63), CommittedKeyId(1));
		malformed.mValid = false;
		FakeStorage::Preload(1, malformed);
		zassert_true(FakePsa::PreloadDurable(CommittedKeyId(1), MakeMaterial(0x64)));
		zassert_true(FakePsa::PreloadDurable(StagedKeyId(1), MakeMaterial(0x65)));
	}

	boot.mHasKeyless = kGlobal >= 3;
	if (boot.mHasKeyless) {
		FakeStorage::Preload(2, MakeRecord(13, kCredentialB, MakeSub(0x66), CommittedKeyId(2)));
	}

	FakePower::ClearTrace();
	return boot;
}

Transitions BootTransitions(const BootCase &boot)
{
	Transitions transitions{};
	if (boot.mHasMalformed) {
		transitions.Add(Erase(1));
	}
	if (boot.mHasKeyless) {
		transitions.Add(Erase(2));
	}
	transitions.Add(Destroy(CommittedKeyId(0)));
	if (boot.mHasMalformed) {
		transitions.Add(Destroy(CommittedKeyId(1)));
		transitions.Add(Destroy(StagedKeyId(1)));
	}
	return transitions;
}

size_t BootPreloadedKeys(const BootCase &boot)
{
	return boot.mHasMalformed ? 4 : 2;
}

/* The kept record resolves whenever its slot was loaded. */
void ExpectKeptResolves(const RowId &id, const BootCase &boot)
{
	zassert_true(Resolves(id, kCredentialA, KeptSub(), boot.mKept.mMaterial, boot.mKept.mRecord),
		     ROW_FMT "the committed record must resolve", ROW_ARGS(id));
}

void FinishBootRow(const RowId &id, const BootCase &boot)
{
	zassert_equal(ALIRO_NO_ERROR, Reboot(), ROW_FMT "recovery must succeed", ROW_ARGS(id));
	for (size_t boots = 0; boots < 2; ++boots) {
		ExpectKeptResolves(id, boot);
		zassert_equal(1u, FakeStorage::PresentCount(), ROW_FMT "only the committed record may remain",
			      ROW_ARGS(id));
		zassert_equal(1u, FakePsa::LiveCount(Kind::Durable), ROW_FMT "only the committed key may remain",
			      ROW_ARGS(id));
		zassert_true(FakePsa::IsLive(StagedKeyId(0), Kind::Durable));
		ExpectNoStrayObjects(id);
		ExpectConsistent();
		if (boots == 0) {
			ExpectStableReboot();
		}
	}
	ExpectCleanTeardown();
}

void ResetBeforeEachTest(void *fixture)
{
	(void)fixture;
	ColdBoot();
}

} // namespace

ZTEST_SUITE(aliro_ud_persistent_key_fault_injection, nullptr, nullptr, ResetBeforeEachTest, nullptr, nullptr);

ZTEST(aliro_ud_persistent_key_fault_injection, test_insert_fault_matrix)
{
	SlotMatrix matrix{ "insert", Op::Insert, false, {}, kInsertRows.data(), kInsertPower.data(),
			   kInsertPresence.data(), kInsertPresence.size() };
	matrix.mTransitions.Add(Own(CommittedKeyId(0)));
	matrix.mTransitions.Add(Save(0));
	RunSlotMatrix(matrix);
}

ZTEST(aliro_ud_persistent_key_fault_injection, test_replacement_fault_matrix)
{
	for (const bool priorAtStaged : { false, true }) {
		const CryptoTypes::KeyId oldKeyId = priorAtStaged ? StagedKeyId(0) : CommittedKeyId(0);
		const CryptoTypes::KeyId newKeyId = priorAtStaged ? CommittedKeyId(0) : StagedKeyId(0);
		SlotMatrix matrix{ priorAtStaged ? "replace-from-staged" : "replace-from-committed",
				   Op::Replace,
				   priorAtStaged,
				   {},
				   kReplaceRows.data(),
				   kReplacePower.data(),
				   kReplacePresence.data(),
				   kReplacePresence.size() };
		matrix.mTransitions.Add(Own(newKeyId));
		matrix.mTransitions.Add(Save(0));
		matrix.mTransitions.Add(Destroy(oldKeyId));
		RunSlotMatrix(matrix);
	}
}

ZTEST(aliro_ud_persistent_key_fault_injection, test_delete_fault_matrix)
{
	for (const bool priorAtStaged : { false, true }) {
		const auto &presence = priorAtStaged ? kDeletePresenceAtStaged : kDeletePresenceAtCommitted;
		SlotMatrix matrix{ priorAtStaged ? "delete-at-staged" : "delete-at-committed",
				   Op::Delete,
				   priorAtStaged,
				   {},
				   kDeleteRows.data(),
				   kDeletePower.data(),
				   presence.data(),
				   presence.size() };
		matrix.mTransitions.Add(Erase(0));
		matrix.mTransitions.Add(Destroy(priorAtStaged ? StagedKeyId(0) : CommittedKeyId(0)));
		RunSlotMatrix(matrix);
	}
}

ZTEST(aliro_ud_persistent_key_fault_injection, test_credential_wide_delete_fault_matrix)
{
	RunBulkMatrix(Bulk::CredentialWide, "delete-all-for-credential");
}

ZTEST(aliro_ud_persistent_key_fault_injection, test_factory_reset_fault_matrix)
{
	RunBulkMatrix(Bulk::FactoryReset, "factory-reset");
}

/* Every durable recovery step, failed before and after its effect, and power cut before it. */
ZTEST(aliro_ud_persistent_key_fault_injection, test_boot_recovery_fault_matrix)
{
	const Transitions transitions = BootTransitions(PrepareBootCase());

	{
		const RowId id{ "boot", "none", 0 };
		const BootCase boot = PrepareBootCase();
		zassert_equal(ALIRO_NO_ERROR, Store::Init());
		ExpectTrace(id, transitions);
		FinishBootRow(id, boot);
	}

	for (size_t k = 0; k < transitions.mCount; ++k) {
		for (const bool withEffect : { false, true }) {
			const RowId id{ "boot", FaultName(withEffect), k };
			const BootCase boot = PrepareBootCase();
			const Transition faulted = transitions.mItems[k];

			FakePower::FailOnceAfter(k, withEffect);
			zassert_not_equal(ALIRO_NO_ERROR, Store::Init(), ROW_FMT "the failure must be returned",
					  ROW_ARGS(id));
			ExpectFaultAt(id, transitions, k);

			/* Recovery continues past the failure; only the faulted step may be left undone. */
			const bool erasePending = !withEffect && faulted.mKind == Mutation::EraseRecord;
			const bool destroyPending = !withEffect && faulted.mKind == Mutation::DestroyKey;
			ExpectKeptResolves(id, boot);
			zassert_equal(erasePending ? 2u : 1u, FakeStorage::PresentCount(), ROW_FMT "unexpected records",
				      ROW_ARGS(id));
			zassert_equal(destroyPending ? 2u : 1u, FakePsa::LiveCount(Kind::Durable),
				      ROW_FMT "unexpected key objects", ROW_ARGS(id));
			if (destroyPending) {
				zassert_true(FakePsa::IsLive(faulted.mTarget, Kind::Durable));
			}
			zassert_true(FakePsa::IsLive(StagedKeyId(0), Kind::Durable));
			ExpectNoStrayObjects(id);

			FinishBootRow(id, boot);
		}
	}

	for (size_t n = 0; n <= transitions.mCount; ++n) {
		const RowId id{ "boot", "power-cut", n };
		const BootCase boot = PrepareBootCase();

		FakePower::CutAfter(n);
		(void)Store::Init();
		zassert_equal(n < transitions.mCount, FakePower::Tripped());
		FinishBootRow(id, boot);
	}
}

/*
 * Read failures abort or degrade recovery without losing the committed
 * record: a persistence init failure changes nothing, a load failure stops
 * before any key is destroyed, and a failed presence query keeps the key.
 */
ZTEST(aliro_ud_persistent_key_fault_injection, test_boot_recovery_read_fault_matrix)
{
	{
		const RowId id{ "boot", "persistence-init", 0 };
		const BootCase boot = PrepareBootCase();
		const FakeStorage::Image image = FakeStorage::Capture();

		FakeStorage::FailNextInit();
		zassert_not_equal(ALIRO_NO_ERROR, Store::Init(), ROW_FMT "the failure must be returned", ROW_ARGS(id));
		zassert_equal(0u, FakePower::TraceLength(), ROW_FMT "nothing may be written", ROW_ARGS(id));
		zassert_true(FakeStorage::Capture() == image);
		zassert_equal(BootPreloadedKeys(boot), FakePsa::LiveCount(Kind::Durable));
		FinishBootRow(id, boot);
	}

	for (size_t slot = 0; slot < kGlobal; ++slot) {
		const RowId id{ "boot", "load", slot };
		const BootCase boot = PrepareBootCase();

		FakeStorage::FailNextLoad(slot);
		zassert_not_equal(ALIRO_NO_ERROR, Store::Init(), ROW_FMT "the failure must be returned", ROW_ARGS(id));
		zassert_false(FakeStorage::ReadFaultArmed());

		/* Only records below the failed slot may have been dropped; no key is destroyed. */
		const size_t erased = (boot.mHasMalformed && slot > 1 ? 1 : 0) + (boot.mHasKeyless && slot > 2 ? 1 : 0);
		zassert_equal(erased, FakePower::TraceLength(), ROW_FMT "unexpected writes", ROW_ARGS(id));
		zassert_equal(BootPreloadedKeys(boot), FakePsa::LiveCount(Kind::Durable),
			      ROW_FMT "no key may be destroyed before every record is loaded", ROW_ARGS(id));
		ExpectNoStrayObjects(id);
		FinishBootRow(id, boot);
	}

	/*
	 * One query per loaded record that may stay (the kept and the keyless
	 * one), then one for the kept slot's other ID and two per other slot.
	 */
	const size_t expectedQueries = 1 + (kGlobal >= 3 ? 1 : 0) + 1 + 2 * (kGlobal - 1);
	for (size_t q = 0;; ++q) {
		const RowId id{ "boot", "presence", q };
		const BootCase boot = PrepareBootCase();

		FakePsa::FailNext(FakePsa::Fault::Exists, q);
		const AliroError error = Store::Init();
		if (FakePsa::FaultArmed()) {
			FakePsa::FailNext(FakePsa::Fault::None);
			zassert_equal(ALIRO_NO_ERROR, error);
			zassert_equal(expectedQueries, q, ROW_FMT "unexpected number of presence queries", ROW_ARGS(id));
			ExpectCleanTeardown();
			break;
		}
		zassert_not_equal(ALIRO_NO_ERROR, error, ROW_FMT "the failure must be returned", ROW_ARGS(id));
		ExpectKeptResolves(id, boot);
		zassert_true(FakePsa::IsLive(StagedKeyId(0), Kind::Durable), ROW_FMT "the committed key must survive",
			     ROW_ARGS(id));
		zassert_equal(0u, FakePsa::DurableCreateCount());
		ExpectNoStrayObjects(id);
		FinishBootRow(id, boot);
	}
}

/*
 * The volatile copy Lookup() mints: a failed copy leaves no object, and a
 * copy outstanding at power loss vanishes without durable change.
 */
ZTEST(aliro_ud_persistent_key_fault_injection, test_lookup_copy_faults)
{
	for (const bool withEffect : { false, true }) {
		const RowId id{ "lookup", FaultName(withEffect), 0 };
		ColdBoot();
		const Stored stored = Insert(kCredentialA, MakeSub(0x70), 0x71);
		const FakeStorage::Image image = FakeStorage::Capture();
		const size_t writes = FakeStorage::WriteCount();

		PK::RecordHandle record{ PK::kInvalidRecordHandle };
		CryptoTypes::KeyId temporary{ 0 };
		if (!withEffect) {
			FakePsa::FailNext(FakePsa::Fault::MintVolatileHandle);
		}
		const AliroError error = PK::Lookup(kCredentialA, MakeSub(0x70), record, temporary);
		zassert_equal(withEffect, error == ALIRO_NO_ERROR, ROW_FMT "unexpected result", ROW_ARGS(id));
		zassert_equal(withEffect ? 1u : 0u, FakePsa::LiveCount(Kind::Temporary));
		if (!withEffect) {
			zassert_equal(PK::kInvalidRecordHandle, record);
			zassert_equal(0u, temporary);
		}

		zassert_equal(ALIRO_NO_ERROR, Reboot());
		zassert_true(FakeStorage::Capture() == image, ROW_FMT "a copy must not change records", ROW_ARGS(id));
		zassert_equal(writes, FakeStorage::WriteCount());
		zassert_equal(1u, FakePsa::LiveCount(Kind::Durable));
		ExpectNoStrayObjects(id);
		zassert_true(Resolves(id, kCredentialA, MakeSub(0x70), stored.mMaterial, stored.mRecord));
		ExpectConsistent();
		ExpectStableReboot();
		ExpectCleanTeardown();
	}
}

/*
 * A save failure whose outcome cannot be read back keeps both keys, so the
 * next boot recovers whichever record persisted: the prior one when the
 * save did not land, the new one when it did.
 */
ZTEST(aliro_ud_persistent_key_fault_injection, test_unreadable_save_outcome_keeps_both_keys)
{
	for (const Op op : { Op::Insert, Op::Replace }) {
		for (const bool withEffect : { false, true }) {
			const RowId id{ op == Op::Insert ? "insert" : "replace", FaultName(withEffect), 1 };
			SlotCase slotCase = PrepareSlotCase(op, false);

			FakePower::FailOnceAfter(1, withEffect);
			FakeStorage::FailNextLoad(0);
			const AliroError error = RunSlotOperation(slotCase);
			zassert_true(FakePower::TraceAt(1) == Save(0));
			zassert_false(FakeStorage::ReadFaultArmed(), ROW_FMT "the save must be read back", ROW_ARGS(id));

			const uint8_t keys = op == Op::Insert ? kNewKey : (kOldKey | kNewKey);
			ExpectSlotRow(id, slotCase, error, Expect{ true, State::Prior, keys, State::Prior });
			FinishSlotRow(id, slotCase, withEffect ? State::New : State::Prior);
		}
	}
}
