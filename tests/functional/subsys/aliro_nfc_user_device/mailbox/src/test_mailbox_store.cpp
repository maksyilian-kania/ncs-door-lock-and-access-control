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
#include "storage/mailbox/mailbox_store.h"

#include <array>

/*
 * AliroUd::Mailbox::Store under test (APP_PLAN.md AWP6): Credential-Issuer-
 * level committed byte storage, live-config lookup from the AWP3 credential
 * store, overflow-safe bounds, idempotent Initialize()/destructive Reset(),
 * and erase-on-credential-delete/-reset integration.
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

CredentialHandle SeedCredentialWithoutMailbox()
{
	ReaderGroupIdentifier readerGroupId{};
	readerGroupId.fill(0x22);

	CryptoTypes::PublicKey trustAnchor{};
	trustAnchor[0] = 0x04;

	AliroUd::Credential::Provisioning::Payload payload{};
	payload.mHasNewKeyInput = true;
	payload.mNewKeyScalar = KeyScalar(0x34);
	payload.mPolicySet = true;
	payload.mBindingCount = 1;
	payload.mBindings[0].mReaderGroupIdentifier = readerGroupId;
	payload.mBindings[0].mTrustType = AliroUd::Credential::TrustType::Direct;
	payload.mBindings[0].mKey = trustAnchor;

	CredentialHandle handle{ kInvalidCredentialHandle };
	const auto error = AliroUd::Credential::Store::Create(payload, handle);
	zassert_equal(ALIRO_NO_ERROR, error, "Seeding the credential without a mailbox must succeed");
	return handle;
}

} // namespace

ZTEST_SUITE(aliro_ud_mailbox_store, nullptr, nullptr, ResetBeforeEachTest, nullptr, nullptr);

ZTEST(aliro_ud_mailbox_store, test_get_config_reflects_live_credential_provisioning)
{
	const auto handle = SeedCredentialWithMailbox(64, /*readable=*/true, /*writable=*/false, /*settable=*/false);

	AliroUd::Mailbox::Store::Config config{};
	zassert_equal(ALIRO_NO_ERROR, AliroUd::Mailbox::Store::GetConfig(handle, config));
	zassert_true(config.mConfigured);
	zassert_equal(64U, config.mSizeBytes);
	zassert_true(config.mPermissions.mReadable);
	zassert_false(config.mPermissions.mWritable);
	zassert_false(config.mPermissions.mSettableInAuth1);
}

ZTEST(aliro_ud_mailbox_store, test_get_config_rejects_credential_without_mailbox)
{
	const auto handle = SeedCredentialWithoutMailbox();

	AliroUd::Mailbox::Store::Config config{};
	zassert_equal(ALIRO_INVALID_ARGUMENT, AliroUd::Mailbox::Store::GetConfig(handle, config));
	zassert_false(config.mConfigured);
}

ZTEST(aliro_ud_mailbox_store, test_get_config_rejects_unknown_handle)
{
	AliroUd::Mailbox::Store::Config config{};
	zassert_not_equal(ALIRO_NO_ERROR, AliroUd::Mailbox::Store::GetConfig(kInvalidCredentialHandle, config));
}

ZTEST(aliro_ud_mailbox_store, test_initialize_is_idempotent_and_zero_fills)
{
	const auto handle = SeedCredentialWithMailbox(16, true, true, false);

	zassert_false(AliroUd::Mailbox::Store::IsInitialized(handle));
	zassert_equal(ALIRO_NO_ERROR, AliroUd::Mailbox::Store::Initialize(handle));
	zassert_true(AliroUd::Mailbox::Store::IsInitialized(handle));
	zassert_false(AliroUd::Mailbox::Store::HasNonZeroData(handle));

	std::array<uint8_t, 16> data{};
	zassert_equal(ALIRO_NO_ERROR, AliroUd::Mailbox::Store::RawRead(handle, 0, data.data(), data.size()));
	for (uint8_t byte : data) {
		zassert_equal(0, byte);
	}

	/* Write some data, then re-Initialize(): must not wipe it (idempotent). */
	std::array<bool, AliroUd::Mailbox::kMaxSizeBytes> dirty{};
	std::array<uint8_t, AliroUd::Mailbox::kMaxSizeBytes> shadow{};
	shadow[0] = 0xAB;
	dirty[0] = true;
	zassert_equal(ALIRO_NO_ERROR, AliroUd::Mailbox::Store::ApplyDirtyBytes(handle, shadow, dirty));

	zassert_equal(ALIRO_NO_ERROR, AliroUd::Mailbox::Store::Initialize(handle));
	zassert_true(AliroUd::Mailbox::Store::HasNonZeroData(handle), "Initialize() must not wipe existing data");
}

ZTEST(aliro_ud_mailbox_store, test_reset_re_zeroes_even_if_already_initialized)
{
	const auto handle = SeedCredentialWithMailbox(16, true, true, false);
	zassert_equal(ALIRO_NO_ERROR, AliroUd::Mailbox::Store::Initialize(handle));

	std::array<bool, AliroUd::Mailbox::kMaxSizeBytes> dirty{};
	std::array<uint8_t, AliroUd::Mailbox::kMaxSizeBytes> shadow{};
	shadow[0] = 0xAB;
	dirty[0] = true;
	zassert_equal(ALIRO_NO_ERROR, AliroUd::Mailbox::Store::ApplyDirtyBytes(handle, shadow, dirty));
	zassert_true(AliroUd::Mailbox::Store::HasNonZeroData(handle));

	zassert_equal(ALIRO_NO_ERROR, AliroUd::Mailbox::Store::Reset(handle));
	zassert_false(AliroUd::Mailbox::Store::HasNonZeroData(handle), "Reset() must re-zero committed data");
	zassert_true(AliroUd::Mailbox::Store::IsInitialized(handle));
}

ZTEST(aliro_ud_mailbox_store, test_raw_read_rejects_out_of_bounds_and_overflowing_ranges)
{
	const auto handle = SeedCredentialWithMailbox(16, true, true, false);
	zassert_equal(ALIRO_NO_ERROR, AliroUd::Mailbox::Store::Initialize(handle));

	std::array<uint8_t, 16> data{};
	zassert_equal(ALIRO_NO_ERROR, AliroUd::Mailbox::Store::RawRead(handle, 0, data.data(), 16));
	zassert_equal(ALIRO_INVALID_ARGUMENT, AliroUd::Mailbox::Store::RawRead(handle, 0, data.data(), 17));
	zassert_equal(ALIRO_INVALID_ARGUMENT, AliroUd::Mailbox::Store::RawRead(handle, 16, data.data(), 1));
	zassert_equal(ALIRO_INVALID_ARGUMENT, AliroUd::Mailbox::Store::RawRead(handle, 10, data.data(), 10));
	/* Overflow-safe: an offset near SIZE_MAX with any positive length must not wrap around and pass bounds. */
	zassert_equal(ALIRO_INVALID_ARGUMENT, AliroUd::Mailbox::Store::RawRead(handle, SIZE_MAX - 4, data.data(), 8));
}

ZTEST(aliro_ud_mailbox_store, test_apply_dirty_bytes_only_touches_dirty_offsets)
{
	const auto handle = SeedCredentialWithMailbox(8, true, true, false);
	zassert_equal(ALIRO_NO_ERROR, AliroUd::Mailbox::Store::Initialize(handle));

	std::array<bool, AliroUd::Mailbox::kMaxSizeBytes> dirty{};
	std::array<uint8_t, AliroUd::Mailbox::kMaxSizeBytes> shadow{};
	shadow[2] = 0x42;
	dirty[2] = true;
	zassert_equal(ALIRO_NO_ERROR, AliroUd::Mailbox::Store::ApplyDirtyBytes(handle, shadow, dirty));

	std::array<uint8_t, 8> data{};
	zassert_equal(ALIRO_NO_ERROR, AliroUd::Mailbox::Store::RawRead(handle, 0, data.data(), data.size()));
	for (size_t i = 0; i < data.size(); ++i) {
		zassert_equal(i == 2 ? 0x42 : 0x00, data[i], "byte %zu unexpected", i);
	}
}

ZTEST(aliro_ud_mailbox_store, test_erase_for_credential_clears_committed_data)
{
	const auto handle = SeedCredentialWithMailbox(8, true, true, false);
	zassert_equal(ALIRO_NO_ERROR, AliroUd::Mailbox::Store::Initialize(handle));

	std::array<bool, AliroUd::Mailbox::kMaxSizeBytes> dirty{};
	std::array<uint8_t, AliroUd::Mailbox::kMaxSizeBytes> shadow{};
	shadow[0] = 0xFF;
	dirty[0] = true;
	zassert_equal(ALIRO_NO_ERROR, AliroUd::Mailbox::Store::ApplyDirtyBytes(handle, shadow, dirty));
	zassert_true(AliroUd::Mailbox::Store::HasNonZeroData(handle));

	zassert_equal(ALIRO_NO_ERROR, AliroUd::Mailbox::Store::EraseForCredential(handle));
	zassert_false(AliroUd::Mailbox::Store::IsInitialized(handle));

	/*
	 * Simulates the handle being reused by a fresh Create() at the same
	 * slot (storage/credential/Kconfig: "handles are never reused for a
	 * different slot", but a *deleted* slot's handle is reused by the
	 * next Create()): the previous credential's mailbox bytes must not
	 * leak into the new one.
	 */
	zassert_equal(ALIRO_NO_ERROR, AliroUd::Credential::Store::Delete(handle));
	const auto newHandle = SeedCredentialWithMailbox(8, true, true, false, /*keySeed=*/0x99);
	zassert_equal(handle, newHandle, "the fake credential backend is expected to reuse the freed slot");
	zassert_equal(ALIRO_NO_ERROR, AliroUd::Mailbox::Store::Initialize(newHandle));
	zassert_false(AliroUd::Mailbox::Store::HasNonZeroData(newHandle),
		      "a new credential at a reused handle must not observe the deleted credential's mailbox data");
}

ZTEST(aliro_ud_mailbox_store, test_erase_all_clears_every_slot)
{
	const auto handleA = SeedCredentialWithMailbox(8, true, true, false, 0x01);
	const auto handleB = SeedCredentialWithMailbox(8, true, true, false, 0x02);
	zassert_equal(ALIRO_NO_ERROR, AliroUd::Mailbox::Store::Initialize(handleA));
	zassert_equal(ALIRO_NO_ERROR, AliroUd::Mailbox::Store::Initialize(handleB));

	zassert_equal(ALIRO_NO_ERROR, AliroUd::Mailbox::Store::EraseAll());

	zassert_false(AliroUd::Mailbox::Store::IsInitialized(handleA));
	zassert_false(AliroUd::Mailbox::Store::IsInitialized(handleB));
}

ZTEST(aliro_ud_mailbox_store, test_persists_across_reboot_simulation)
{
	const auto handle = SeedCredentialWithMailbox(8, true, true, false);
	zassert_equal(ALIRO_NO_ERROR, AliroUd::Mailbox::Store::Initialize(handle));

	std::array<bool, AliroUd::Mailbox::kMaxSizeBytes> dirty{};
	std::array<uint8_t, AliroUd::Mailbox::kMaxSizeBytes> shadow{};
	shadow[3] = 0x55;
	dirty[3] = true;
	zassert_equal(ALIRO_NO_ERROR, AliroUd::Mailbox::Store::ApplyDirtyBytes(handle, shadow, dirty));

	/* Simulate reboot: re-run Init() without clearing the fake's backing storage. */
	zassert_equal(ALIRO_NO_ERROR, AliroUd::Mailbox::Store::Init());

	std::array<uint8_t, 8> data{};
	zassert_equal(ALIRO_NO_ERROR, AliroUd::Mailbox::Store::RawRead(handle, 0, data.data(), data.size()));
	zassert_equal(0x55, data[3], "committed mailbox data must survive a reboot");
}
