/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include <zephyr/ztest.h>

#include "fake_credential_persistence.h"
#include "fake_key_backend.h"
#include "fake_mailbox_persistence.h"
#include "storage/credential/credential_store.h"
#include "storage/mailbox/mailbox_sessions.h"
#include "storage/mailbox/mailbox_store.h"

#include <array>

/*
 * Injected-failure coverage for AliroUd::Mailbox::Store/Sessions
 * (APP_PLAN.md AWP6: "Test recovery from reset/power-loss injection at
 * each mailbox commit transition"): a persistence failure at the single
 * atomic write each mutating operation performs must leave the previously
 * committed record completely unchanged, and a subsequent "reboot"
 * (re-running Store::Init()) must recover the last successfully committed
 * state, not a partially-applied one.
 */

using namespace Aliro;
using namespace Aliro::UserDevice;

namespace {

void ResetBeforeEachTest(void *fixture)
{
	(void)fixture;

	AliroUd::Credential::Test::ResetFakePersistence();
	AliroUd::Credential::Test::ResetFakeKeyBackend();
	AliroUd::Mailbox::Test::ResetFakeMailboxPersistence();

	zassert_equal(ALIRO_NO_ERROR, AliroUd::Credential::Store::Init(), "Credential store init must succeed");
	zassert_equal(ALIRO_NO_ERROR, AliroUd::Mailbox::Store::Init(), "Mailbox store init must succeed");
}

CredentialHandle SeedCredentialWithMailbox(uint32_t sizeBytes)
{
	ReaderGroupIdentifier readerGroupId{};
	readerGroupId.fill(0x11);

	CryptoTypes::PublicKey trustAnchor{};
	trustAnchor[0] = 0x04;

	AliroUd::Credential::Provisioning::Payload payload{};
	payload.mHasNewKeyInput = true;
	payload.mNewKeyScalar = { 0x23, 0x23, 0x10, 0x22, 0xa3, 0x66, 0x2c, 0xeb, 0x6f, 0x2e, 0x6a, 0x4e,
				  0x99, 0x88, 0x66, 0xae, 0x88, 0xd6, 0xe9, 0xda, 0x1c, 0x72, 0xb0, 0x50,
				  0xae, 0x5c, 0x20, 0x6a, 0x1d, 0xa4, 0x67, 0x12 };
	payload.mPolicySet = true;
	payload.mBindingCount = 1;
	payload.mBindings[0].mReaderGroupIdentifier = readerGroupId;
	payload.mBindings[0].mTrustType = AliroUd::Credential::TrustType::Direct;
	payload.mBindings[0].mKey = trustAnchor;
	payload.mMailbox.mConfigured = true;
	payload.mMailbox.mSizeBytes = sizeBytes;
	payload.mMailbox.mReadable = true;
	payload.mMailbox.mWritable = true;

	CredentialHandle handle{ kInvalidCredentialHandle };
	const auto error = AliroUd::Credential::Store::Create(payload, handle);
	zassert_equal(ALIRO_NO_ERROR, error, "Seeding the credential with a mailbox must succeed");
	return handle;
}

::Aliro::UserDevice::MailboxHandle MailboxHandleOf(CredentialHandle credentialHandle)
{
	return AliroUd::Mailbox::Store::HandleForCredential(credentialHandle);
}

} // namespace

ZTEST_SUITE(aliro_ud_mailbox_fault_injection, nullptr, nullptr, ResetBeforeEachTest, nullptr, nullptr);

ZTEST(aliro_ud_mailbox_fault_injection, test_initialize_failure_leaves_uninitialized_state)
{
	const auto handle = SeedCredentialWithMailbox(8);

	AliroUd::Mailbox::Test::ArmMailboxFault(AliroUd::Mailbox::Test::FaultPoint::SaveSlot);
	zassert_not_equal(ALIRO_NO_ERROR, AliroUd::Mailbox::Store::Initialize(handle));
	zassert_false(AliroUd::Mailbox::Store::IsInitialized(handle));

	/* Retrying without the fault armed must succeed (no orphaned/half-applied state blocks recovery). */
	zassert_equal(ALIRO_NO_ERROR, AliroUd::Mailbox::Store::Initialize(handle));
	zassert_true(AliroUd::Mailbox::Store::IsInitialized(handle));
}

ZTEST(aliro_ud_mailbox_fault_injection, test_commit_failure_leaves_committed_bytes_unchanged)
{
	const auto handle = SeedCredentialWithMailbox(8);

	AliroUd::Mailbox::Sessions::SessionHandle session{};
	zassert_equal(ALIRO_NO_ERROR, AliroUd::Mailbox::Sessions::OpenSnapshot(MailboxHandleOf(handle), session));

	const std::array<uint8_t, 4> first{ 0x11, 0x22, 0x33, 0x44 };
	zassert_equal(ALIRO_NO_ERROR, AliroUd::Mailbox::Sessions::StageWrite(session, 0, first.data(), first.size()));
	zassert_equal(ALIRO_NO_ERROR, AliroUd::Mailbox::Sessions::Commit(session));

	/* Stage a second write, then fail its Commit(): the first commit's bytes must survive untouched. */
	const std::array<uint8_t, 4> second{ 0xAA, 0xBB, 0xCC, 0xDD };
	zassert_equal(ALIRO_NO_ERROR, AliroUd::Mailbox::Sessions::StageWrite(session, 0, second.data(), second.size()));

	AliroUd::Mailbox::Test::ArmMailboxFault(AliroUd::Mailbox::Test::FaultPoint::SaveSlot);
	zassert_not_equal(ALIRO_NO_ERROR, AliroUd::Mailbox::Sessions::Commit(session));

	std::array<uint8_t, 4> readBack{};
	zassert_equal(ALIRO_NO_ERROR, AliroUd::Mailbox::Sessions::Read(session, 0, readBack.data(), readBack.size()));
	zassert_true(readBack == first, "a failed Commit() must leave the previously committed bytes unchanged");

	/* The failed commit's staged data is preserved (not discarded) so the caller can retry. */
	zassert_equal(ALIRO_NO_ERROR, AliroUd::Mailbox::Sessions::Commit(session));
	zassert_equal(ALIRO_NO_ERROR, AliroUd::Mailbox::Sessions::Read(session, 0, readBack.data(), readBack.size()));
	zassert_true(readBack == second, "retrying Commit() without the fault armed must succeed");

	AliroUd::Mailbox::Sessions::Close(session);
}

ZTEST(aliro_ud_mailbox_fault_injection, test_reboot_after_commit_failure_recovers_last_committed_state)
{
	const auto handle = SeedCredentialWithMailbox(8);

	AliroUd::Mailbox::Sessions::SessionHandle session{};
	zassert_equal(ALIRO_NO_ERROR, AliroUd::Mailbox::Sessions::OpenSnapshot(MailboxHandleOf(handle), session));

	const std::array<uint8_t, 4> committed{ 0x01, 0x02, 0x03, 0x04 };
	zassert_equal(ALIRO_NO_ERROR,
		     AliroUd::Mailbox::Sessions::StageWrite(session, 0, committed.data(), committed.size()));
	zassert_equal(ALIRO_NO_ERROR, AliroUd::Mailbox::Sessions::Commit(session));

	const std::array<uint8_t, 4> lost{ 0xFF, 0xFF, 0xFF, 0xFF };
	zassert_equal(ALIRO_NO_ERROR, AliroUd::Mailbox::Sessions::StageWrite(session, 0, lost.data(), lost.size()));

	AliroUd::Mailbox::Test::ArmMailboxFault(AliroUd::Mailbox::Test::FaultPoint::SaveSlot);
	zassert_not_equal(ALIRO_NO_ERROR, AliroUd::Mailbox::Sessions::Commit(session));
	AliroUd::Mailbox::Sessions::Close(session);

	/* Simulate power loss right after the failed commit, then reboot. */
	zassert_equal(ALIRO_NO_ERROR, AliroUd::Mailbox::Store::Init());

	std::array<uint8_t, 4> readBack{};
	zassert_equal(ALIRO_NO_ERROR, AliroUd::Mailbox::Store::RawRead(handle, 0, readBack.data(), readBack.size()));
	zassert_true(readBack == committed, "a reboot after a failed commit must recover the last committed bytes");
}

ZTEST(aliro_ud_mailbox_fault_injection, test_erase_failure_leaves_slot_initialized)
{
	const auto handle = SeedCredentialWithMailbox(8);
	zassert_equal(ALIRO_NO_ERROR, AliroUd::Mailbox::Store::Initialize(handle));

	AliroUd::Mailbox::Test::ArmMailboxFault(AliroUd::Mailbox::Test::FaultPoint::EraseSlot);
	zassert_not_equal(ALIRO_NO_ERROR, AliroUd::Mailbox::Store::EraseForCredential(handle));
	zassert_true(AliroUd::Mailbox::Store::IsInitialized(handle),
		     "a failed erase must not leave the in-memory state ahead of persisted state");

	/* Retrying without the fault armed must succeed. */
	zassert_equal(ALIRO_NO_ERROR, AliroUd::Mailbox::Store::EraseForCredential(handle));
	zassert_false(AliroUd::Mailbox::Store::IsInitialized(handle));
}
