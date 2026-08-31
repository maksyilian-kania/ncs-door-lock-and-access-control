/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include <zephyr/ztest.h>

#include "fake_credential_persistence.h"
#include "fake_key_backend.h"
#include "storage/credential/credential_store.h"
#include "storage/credential/key_backend.h"

#include <aliro/user_device/interface.h>

#include <array>

/*
 * Aliro::Interface::UserDevice::CredentialSigning::Sign() under test
 * (APP_PLAN.md AWP5, platform/crypto/credential_signing.cpp), end to end
 * against a real AWP3 credential provisioned through credential_store.cpp
 * (with the host-test fake persistence/key backends): resolves a
 * CredentialHandle to its PSA key via credential_store.h's GetFullRecord(),
 * signs through KeyBackend::Sign(), and the resulting signature is checked
 * against the credential's own public key via
 * Aliro::Interface::UserDevice::Crypto::VerifySignature() (platform/crypto/crypto.cpp).
 */

using namespace Aliro;

namespace {

void ResetBeforeEachTest(void *fixture)
{
	(void)fixture;

	AliroUd::Credential::Test::ResetFakePersistence();
	AliroUd::Credential::Test::ResetFakeKeyBackend();

	zassert_equal(ALIRO_NO_ERROR, AliroUd::Credential::Store::Init(), "Credential store init must succeed");
}

/* An arbitrary valid (nonzero, below the P-256 curve order) private key scalar; reused from the AWP3 CLI test. */
std::array<uint8_t, 32> KeyScalar()
{
	return { 0x23, 0x23, 0x10, 0x22, 0xa3, 0x66, 0x2c, 0xeb, 0x6f, 0x2e, 0x6a, 0x4e, 0x99, 0x88, 0x66, 0xae,
		 0x88, 0xd6, 0xe9, 0xda, 0x1c, 0x72, 0xb0, 0x50, 0xae, 0x5c, 0x20, 0x6a, 0x1d, 0xa4, 0x67, 0x12 };
}

::Aliro::UserDevice::CredentialHandle SeedCredential()
{
	::Aliro::UserDevice::ReaderGroupIdentifier readerGroupId{};
	readerGroupId.fill(0x11);

	CryptoTypes::PublicKey trustAnchor{};
	trustAnchor[0] = 0x04;

	AliroUd::Credential::Provisioning::Payload payload{};
	payload.mHasNewKeyInput = true;
	payload.mNewKeyScalar = KeyScalar();
	payload.mPolicySet = true;
	payload.mPolicy = ::Aliro::UserDevice::AuthenticationPolicy::UserDeviceSetting;
	payload.mBindingCount = 1;
	payload.mBindings[0].mReaderGroupIdentifier = readerGroupId;
	payload.mBindings[0].mTrustType = AliroUd::Credential::TrustType::Direct;
	payload.mBindings[0].mKey = trustAnchor;

	::Aliro::UserDevice::CredentialHandle handle{ ::Aliro::UserDevice::kInvalidCredentialHandle };
	const auto error = AliroUd::Credential::Store::Create(payload, handle);
	zassert_equal(ALIRO_NO_ERROR, error, "Seeding the credential must succeed");
	return handle;
}

} // namespace

ZTEST_SUITE(aliro_ud_credential_signing, nullptr, nullptr, ResetBeforeEachTest, nullptr, nullptr);

ZTEST(aliro_ud_credential_signing, test_sign_produces_signature_verifiable_against_credential_public_key)
{
	const auto handle = SeedCredential();

	AliroUd::Credential::PersistedCredential record{};
	zassert_equal(ALIRO_NO_ERROR, AliroUd::Credential::Store::GetFullRecord(handle, record));

	CryptoTypes::PublicKey publicKey{};
	zassert_equal(ALIRO_NO_ERROR, AliroUd::Credential::KeyBackend::GetPublicKey(record.mKeyId, publicKey));

	const std::array<uint8_t, 6> data{ 'a', 'l', 'i', 'r', 'o', '!' };
	CryptoTypes::Signature signature{};
	const AliroError error = Interface::UserDevice::CredentialSigning::Sign(handle, data.data(), data.size(),
									       signature);
	zassert_equal(ALIRO_NO_ERROR, error);

	zassert_equal(ALIRO_NO_ERROR,
		     Interface::UserDevice::Crypto::VerifySignature(publicKey, data.data(), data.size(), signature));
}

ZTEST(aliro_ud_credential_signing, test_sign_rejects_invalid_handle)
{
	CryptoTypes::Signature signature{};
	signature.fill(0xAA);
	const std::array<uint8_t, 4> data{ 't', 'e', 's', 't' };

	const AliroError error = Interface::UserDevice::CredentialSigning::Sign(
		::Aliro::UserDevice::kInvalidCredentialHandle, data.data(), data.size(), signature);

	zassert_not_equal(ALIRO_NO_ERROR, error);
	for (uint8_t byte : signature) {
		zassert_equal(0, byte, "outSignature must be cleared on failure");
	}
}

ZTEST(aliro_ud_credential_signing, test_sign_rejects_empty_data)
{
	const auto handle = SeedCredential();
	CryptoTypes::Signature signature{};

	const AliroError error = Interface::UserDevice::CredentialSigning::Sign(handle, nullptr, 0, signature);

	zassert_equal(ALIRO_INVALID_ARGUMENT, error);
}
