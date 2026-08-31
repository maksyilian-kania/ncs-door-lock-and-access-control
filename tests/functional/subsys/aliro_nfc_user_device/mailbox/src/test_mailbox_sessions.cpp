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

#include <aliro/user_device/interface.h>

#include <array>

/*
 * AliroUd::Mailbox::Sessions under test (APP_PLAN.md AWP6): the
 * Reader-facing OpenSnapshot/Read/StageWrite/StageSet/Commit/Rollback/Close
 * contract, exercised both directly (AliroUd::Mailbox::Sessions::*) and
 * through the real Aliro::Interface::UserDevice::Mailbox adapter
 * (mailbox.cpp) to prove the thin-adapter forwarding is exact — same split
 * as test_authorization_contract.cpp over authorization.cpp.
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

std::array<uint8_t, 32> KeyScalar(uint8_t seed)
{
	std::array<uint8_t, 32> scalar{ 0x23, 0x23, 0x10, 0x22, 0xa3, 0x66, 0x2c, 0xeb, 0x6f, 0x2e,
					 0x6a, 0x4e, 0x99, 0x88, 0x66, 0xae, 0x88, 0xd6, 0xe9, 0xda,
					 0x1c, 0x72, 0xb0, 0x50, 0xae, 0x5c, 0x20, 0x6a, 0x1d, 0xa4,
					 0x67, seed };
	return scalar;
}

CredentialHandle SeedCredentialWithMailbox(uint32_t sizeBytes, bool readable, bool writable, bool settableInAuth1,
					   uint8_t keySeed = 0x12)
{
	ReaderGroupIdentifier readerGroupId{};
	readerGroupId.fill(0x11);

	CryptoTypes::PublicKey trustAnchor{};
	trustAnchor[0] = 0x04;

	AliroUd::Credential::Provisioning::Payload payload{};
	payload.mHasNewKeyInput = true;
	payload.mNewKeyScalar = KeyScalar(keySeed);
	payload.mPolicySet = true;
	payload.mPolicy = AuthenticationPolicy::UserDeviceSetting;
	payload.mBindingCount = 1;
	payload.mBindings[0].mReaderGroupIdentifier = readerGroupId;
	payload.mBindings[0].mTrustType = AliroUd::Credential::TrustType::Direct;
	payload.mBindings[0].mKey = trustAnchor;
	payload.mMailbox.mConfigured = true;
	payload.mMailbox.mSizeBytes = sizeBytes;
	payload.mMailbox.mReadable = readable;
	payload.mMailbox.mWritable = writable;
	payload.mMailbox.mSettableInAuth1 = settableInAuth1;

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

ZTEST_SUITE(aliro_ud_mailbox_sessions, nullptr, nullptr, ResetBeforeEachTest, nullptr, nullptr);

ZTEST(aliro_ud_mailbox_sessions, test_open_snapshot_rejects_credential_without_configured_mailbox)
{
	AliroUd::Mailbox::Sessions::SessionHandle session{};
	const auto error = AliroUd::Mailbox::Sessions::OpenSnapshot(MailboxHandleOf(kInvalidCredentialHandle), session);
	zassert_not_equal(ALIRO_NO_ERROR, error);
	zassert_equal(AliroUd::Mailbox::Sessions::kInvalidSessionHandle, session);
}

ZTEST(aliro_ud_mailbox_sessions, test_open_snapshot_lazily_initializes_committed_storage)
{
	const auto handle = SeedCredentialWithMailbox(8, true, true, false);
	zassert_false(AliroUd::Mailbox::Store::IsInitialized(handle));

	AliroUd::Mailbox::Sessions::SessionHandle session{};
	zassert_equal(ALIRO_NO_ERROR, AliroUd::Mailbox::Sessions::OpenSnapshot(MailboxHandleOf(handle), session));
	zassert_not_equal(AliroUd::Mailbox::Sessions::kInvalidSessionHandle, session);
	zassert_true(AliroUd::Mailbox::Store::IsInitialized(handle));

	AliroUd::Mailbox::Sessions::Close(session);
}

ZTEST(aliro_ud_mailbox_sessions, test_read_isolated_from_staged_writes_until_commit)
{
	const auto handle = SeedCredentialWithMailbox(8, /*readable=*/true, /*writable=*/true, false);

	AliroUd::Mailbox::Sessions::SessionHandle session{};
	zassert_equal(ALIRO_NO_ERROR, AliroUd::Mailbox::Sessions::OpenSnapshot(MailboxHandleOf(handle), session));

	const std::array<uint8_t, 3> written{ 0x11, 0x22, 0x33 };
	zassert_equal(ALIRO_NO_ERROR, AliroUd::Mailbox::Sessions::StageWrite(session, 0, written.data(), written.size()));

	std::array<uint8_t, 3> readBack{ 0xAA, 0xAA, 0xAA };
	zassert_equal(ALIRO_NO_ERROR, AliroUd::Mailbox::Sessions::Read(session, 0, readBack.data(), readBack.size()));
	for (uint8_t byte : readBack) {
		zassert_equal(0x00, byte, "Read() must observe only committed bytes before Commit()");
	}

	zassert_equal(ALIRO_NO_ERROR, AliroUd::Mailbox::Sessions::Commit(session));
	zassert_equal(ALIRO_NO_ERROR, AliroUd::Mailbox::Sessions::Read(session, 0, readBack.data(), readBack.size()));
	zassert_true(readBack == written, "Read() must observe the committed bytes after Commit()");

	AliroUd::Mailbox::Sessions::Close(session);
}

ZTEST(aliro_ud_mailbox_sessions, test_rollback_and_close_leave_committed_bytes_unchanged)
{
	const auto handle = SeedCredentialWithMailbox(8, true, true, false);

	AliroUd::Mailbox::Sessions::SessionHandle session{};
	zassert_equal(ALIRO_NO_ERROR, AliroUd::Mailbox::Sessions::OpenSnapshot(MailboxHandleOf(handle), session));

	const std::array<uint8_t, 3> committed{ 0x01, 0x02, 0x03 };
	zassert_equal(ALIRO_NO_ERROR,
		     AliroUd::Mailbox::Sessions::StageWrite(session, 0, committed.data(), committed.size()));
	zassert_equal(ALIRO_NO_ERROR, AliroUd::Mailbox::Sessions::Commit(session));

	const std::array<uint8_t, 3> attempted{ 0xFF, 0xFF, 0xFF };
	zassert_equal(ALIRO_NO_ERROR,
		     AliroUd::Mailbox::Sessions::StageWrite(session, 0, attempted.data(), attempted.size()));
	zassert_equal(ALIRO_NO_ERROR, AliroUd::Mailbox::Sessions::Rollback(session));

	std::array<uint8_t, 3> readBack{};
	zassert_equal(ALIRO_NO_ERROR, AliroUd::Mailbox::Sessions::Read(session, 0, readBack.data(), readBack.size()));
	zassert_true(readBack == committed, "Rollback() must discard staged data without touching committed bytes");

	/* A second, independent session must also observe only the previously committed bytes after Close(). */
	AliroUd::Mailbox::Sessions::Close(session);

	AliroUd::Mailbox::Sessions::SessionHandle session2{};
	zassert_equal(ALIRO_NO_ERROR, AliroUd::Mailbox::Sessions::OpenSnapshot(MailboxHandleOf(handle), session2));
	zassert_equal(ALIRO_NO_ERROR,
		     AliroUd::Mailbox::Sessions::StageWrite(session2, 0, attempted.data(), attempted.size()));
	AliroUd::Mailbox::Sessions::Close(session2); /* Close() without Commit() must also leave committed data unchanged. */

	AliroUd::Mailbox::Sessions::SessionHandle session3{};
	zassert_equal(ALIRO_NO_ERROR, AliroUd::Mailbox::Sessions::OpenSnapshot(MailboxHandleOf(handle), session3));
	zassert_equal(ALIRO_NO_ERROR, AliroUd::Mailbox::Sessions::Read(session3, 0, readBack.data(), readBack.size()));
	zassert_true(readBack == committed, "Close() without Commit() must leave committed bytes unchanged");
	AliroUd::Mailbox::Sessions::Close(session3);
}

ZTEST(aliro_ud_mailbox_sessions, test_read_rejected_when_not_readable)
{
	const auto handle = SeedCredentialWithMailbox(8, /*readable=*/false, /*writable=*/true, false);

	AliroUd::Mailbox::Sessions::SessionHandle session{};
	zassert_equal(ALIRO_NO_ERROR, AliroUd::Mailbox::Sessions::OpenSnapshot(MailboxHandleOf(handle), session));

	uint8_t byte{};
	zassert_not_equal(ALIRO_NO_ERROR, AliroUd::Mailbox::Sessions::Read(session, 0, &byte, 1));

	AliroUd::Mailbox::Sessions::Close(session);
}

ZTEST(aliro_ud_mailbox_sessions, test_stage_write_rejected_when_not_writable)
{
	const auto handle = SeedCredentialWithMailbox(8, /*readable=*/true, /*writable=*/false, false);

	AliroUd::Mailbox::Sessions::SessionHandle session{};
	zassert_equal(ALIRO_NO_ERROR, AliroUd::Mailbox::Sessions::OpenSnapshot(MailboxHandleOf(handle), session));

	uint8_t byte{ 0x01 };
	zassert_not_equal(ALIRO_NO_ERROR, AliroUd::Mailbox::Sessions::StageWrite(session, 0, &byte, 1));

	AliroUd::Mailbox::Sessions::Close(session);
}

ZTEST(aliro_ud_mailbox_sessions, test_stage_write_rejects_out_of_bounds_and_overflowing_ranges)
{
	const auto handle = SeedCredentialWithMailbox(8, true, true, false);

	AliroUd::Mailbox::Sessions::SessionHandle session{};
	zassert_equal(ALIRO_NO_ERROR, AliroUd::Mailbox::Sessions::OpenSnapshot(MailboxHandleOf(handle), session));

	std::array<uint8_t, 4> data{};
	zassert_equal(ALIRO_NO_ERROR, AliroUd::Mailbox::Sessions::StageWrite(session, 4, data.data(), 4));
	zassert_equal(ALIRO_INVALID_ARGUMENT, AliroUd::Mailbox::Sessions::StageWrite(session, 5, data.data(), 4));
	zassert_equal(ALIRO_INVALID_ARGUMENT, AliroUd::Mailbox::Sessions::StageWrite(session, 8, data.data(), 1));
	zassert_equal(ALIRO_INVALID_ARGUMENT,
		     AliroUd::Mailbox::Sessions::StageWrite(session, SIZE_MAX - 2, data.data(), 4));

	AliroUd::Mailbox::Sessions::Close(session);
}

ZTEST(aliro_ud_mailbox_sessions, test_stage_set_requires_exact_full_mailbox_length)
{
	const auto handle = SeedCredentialWithMailbox(4, true, true, false);

	AliroUd::Mailbox::Sessions::SessionHandle session{};
	zassert_equal(ALIRO_NO_ERROR, AliroUd::Mailbox::Sessions::OpenSnapshot(MailboxHandleOf(handle), session));

	const std::array<uint8_t, 3> tooShort{};
	zassert_equal(ALIRO_INVALID_ARGUMENT, AliroUd::Mailbox::Sessions::StageSet(session, tooShort.data(), 3));

	const std::array<uint8_t, 4> exact{ 0x7, 0x7, 0x7, 0x7 };
	zassert_equal(ALIRO_NO_ERROR, AliroUd::Mailbox::Sessions::StageSet(session, exact.data(), 4));
	zassert_equal(ALIRO_NO_ERROR, AliroUd::Mailbox::Sessions::Commit(session));

	std::array<uint8_t, 4> readBack{};
	zassert_equal(ALIRO_NO_ERROR, AliroUd::Mailbox::Sessions::Read(session, 0, readBack.data(), readBack.size()));
	zassert_true(readBack == exact);

	AliroUd::Mailbox::Sessions::Close(session);
}

ZTEST(aliro_ud_mailbox_sessions, test_multiple_sessions_stage_independently)
{
	const auto handle = SeedCredentialWithMailbox(4, true, true, false);

	AliroUd::Mailbox::Sessions::SessionHandle sessionA{};
	AliroUd::Mailbox::Sessions::SessionHandle sessionB{};
	zassert_equal(ALIRO_NO_ERROR, AliroUd::Mailbox::Sessions::OpenSnapshot(MailboxHandleOf(handle), sessionA));
	zassert_equal(ALIRO_NO_ERROR, AliroUd::Mailbox::Sessions::OpenSnapshot(MailboxHandleOf(handle), sessionB));
	zassert_not_equal(sessionA, sessionB);

	uint8_t byteA{ 0xAA };
	uint8_t byteB{ 0xBB };
	zassert_equal(ALIRO_NO_ERROR, AliroUd::Mailbox::Sessions::StageWrite(sessionA, 0, &byteA, 1));
	zassert_equal(ALIRO_NO_ERROR, AliroUd::Mailbox::Sessions::StageWrite(sessionB, 0, &byteB, 1));

	/* Only sessionA's staged byte is committed; sessionB's own staged copy must be unaffected. */
	zassert_equal(ALIRO_NO_ERROR, AliroUd::Mailbox::Sessions::Commit(sessionA));

	uint8_t readBack{};
	zassert_equal(ALIRO_NO_ERROR, AliroUd::Mailbox::Sessions::Read(sessionA, 0, &readBack, 1));
	zassert_equal(0xAA, readBack);

	zassert_equal(ALIRO_NO_ERROR, AliroUd::Mailbox::Sessions::Commit(sessionB));
	zassert_equal(ALIRO_NO_ERROR, AliroUd::Mailbox::Sessions::Read(sessionA, 0, &readBack, 1));
	zassert_equal(0xBB, readBack, "sessionB's independently staged byte must win once it commits");

	AliroUd::Mailbox::Sessions::Close(sessionA);
	AliroUd::Mailbox::Sessions::Close(sessionB);
}

ZTEST(aliro_ud_mailbox_sessions, test_operations_on_unknown_session_fail_safely)
{
	uint8_t byte{};
	zassert_not_equal(ALIRO_NO_ERROR, AliroUd::Mailbox::Sessions::Read(0xDEAD, 0, &byte, 1));
	zassert_not_equal(ALIRO_NO_ERROR, AliroUd::Mailbox::Sessions::StageWrite(0xDEAD, 0, &byte, 1));
	zassert_not_equal(ALIRO_NO_ERROR, AliroUd::Mailbox::Sessions::Commit(0xDEAD));
	zassert_not_equal(ALIRO_NO_ERROR, AliroUd::Mailbox::Sessions::Rollback(0xDEAD));
	/* Close() on an unknown/already-closed session is a documented no-op, not an error. */
	AliroUd::Mailbox::Sessions::Close(0xDEAD);
}

ZTEST(aliro_ud_mailbox_sessions, test_shrinking_mailbox_mid_session_is_reflected_immediately)
{
	const auto handle = SeedCredentialWithMailbox(8, true, true, false);

	AliroUd::Mailbox::Sessions::SessionHandle session{};
	zassert_equal(ALIRO_NO_ERROR, AliroUd::Mailbox::Sessions::OpenSnapshot(MailboxHandleOf(handle), session));

	/* Shrink the provisioned mailbox size via a credential update, mid-session. */
	AliroUd::Credential::PersistedCredential record{};
	zassert_equal(ALIRO_NO_ERROR, AliroUd::Credential::Store::GetFullRecord(handle, record));

	AliroUd::Credential::Provisioning::Payload update{};
	update.mPolicySet = true;
	update.mPolicy = record.mPolicy;
	update.mBindingCount = record.mBindingCount;
	update.mBindings = record.mBindings;
	update.mMailbox = record.mMailbox;
	update.mMailbox.mSizeBytes = 2;
	zassert_equal(ALIRO_NO_ERROR, AliroUd::Credential::Store::Update(handle, update));

	uint8_t data{ 0x01 };
	zassert_equal(ALIRO_INVALID_ARGUMENT, AliroUd::Mailbox::Sessions::StageWrite(session, 4, &data, 1),
		     "an offset valid before the shrink must now be rejected");

	AliroUd::Mailbox::Sessions::Close(session);
}

/*
 * The same behavior once more through the real
 * Aliro::Interface::UserDevice::Mailbox adapter (mailbox.cpp), proving the
 * adapter forwards every parameter/return value unchanged.
 */
ZTEST(aliro_ud_mailbox_sessions, test_interface_adapter_forwards_to_sessions_engine)
{
	const auto handle = SeedCredentialWithMailbox(4, true, true, false);

	Interface::UserDevice::Mailbox::SessionHandle session{ Interface::UserDevice::Mailbox::kInvalidSessionHandle };
	zassert_equal(ALIRO_NO_ERROR, Interface::UserDevice::Mailbox::OpenSnapshot(MailboxHandleOf(handle), session));
	zassert_not_equal(Interface::UserDevice::Mailbox::kInvalidSessionHandle, session);

	const std::array<uint8_t, 4> full{ 0x9, 0x9, 0x9, 0x9 };
	zassert_equal(ALIRO_NO_ERROR, Interface::UserDevice::Mailbox::StageSet(session, full.data(), full.size()));
	zassert_equal(ALIRO_NO_ERROR, Interface::UserDevice::Mailbox::Commit(session));

	std::array<uint8_t, 4> readBack{};
	zassert_equal(ALIRO_NO_ERROR,
		     Interface::UserDevice::Mailbox::Read(session, 0, readBack.data(), readBack.size()));
	zassert_true(readBack == full);

	Interface::UserDevice::Mailbox::Close(session);
}
