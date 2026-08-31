/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include <zephyr/kernel.h>
#include <zephyr/ztest.h>

#include "fake_authorization_indicator.h"
#include "fake_credential_persistence.h"
#include "fake_key_backend.h"
#include "fake_nfc_interface.h"
#include "platform/authorization/authorization_window.h"
#include "platform/nfc/nfc_worker.h"
#include "storage/credential/credential_store.h"

#include <aliro/user_device/interface.h>

#include <array>
#include <vector>

/*
 * End-to-end test: a real AUTH0 command, with authentication_policy 0x03
 * ("Force user authentication"), sent through the real bounded queue/worker
 * thread and the real Aliro::UserDeviceStack (ncs-aliro WP5.5) against a
 * real credential provisioned through this application's own
 * credential_store, exercising the full application-owned half of
 * ALIRO-UD-SYRS-P1-011/012/020/021: no window -> AUTH0 defers exactly one
 * NotifyAuthenticationRequired() call (turning the LED indication on);
 * pressing the button (Window::Open()) first -> the same AUTH0 command
 * proceeds without ever calling it.
 *
 * ncs-aliro WP6 does not implement AUTH0 cryptography yet, so the wire
 * response is the same empty-data failure status word in both cases
 * (ALIRO-UD-SYRS-P1-031: "AUTH0 ... externally observable data independent
 * of credential/reader-key/Kpersistent existence"); only the deferred
 * NotifyAuthenticationRequired() delivery differs, which is exactly the
 * application-owned behavior AWP4 implements.
 */

using namespace AliroUd::Nfc;

namespace {

void SettleWorker()
{
	k_msleep(50);
}

void *SetupSuite(void)
{
	StartWorker();
	SettleWorker();
	return nullptr;
}

void ResetBeforeEachTest(void *fixture)
{
	(void)fixture;

	AliroUdTest::FakeNfc::Reset();
	AliroUd::Credential::Test::ResetFakePersistence();
	AliroUd::Credential::Test::ResetFakeKeyBackend();
	AliroUd::Authorization::GlobalWindow().Close();
	AliroUd::Authorization::Test::ResetFakeAuthorizationIndicator();

	zassert_equal(ALIRO_NO_ERROR, AliroUd::Credential::Store::Init(), "Credential store init must succeed");

	if (IsSessionActive()) {
		PostFieldOff();
		SettleWorker();
	}
}

/* An arbitrary valid (nonzero, below the P-256 curve order) private key scalar; reused from the AWP3 CLI test. */
std::array<uint8_t, 32> KeyScalar()
{
	return { 0x23, 0x23, 0x10, 0x22, 0xa3, 0x66, 0x2c, 0xeb, 0x6f, 0x2e, 0x6a, 0x4e, 0x99, 0x88, 0x66, 0xae,
		 0x88, 0xd6, 0xe9, 0xda, 0x1c, 0x72, 0xb0, 0x50, 0xae, 0x5c, 0x20, 0x6a, 0x1d, 0xa4, 0x67, 0x12 };
}

::Aliro::CryptoTypes::PublicKey ArbitraryTrustAnchorKey()
{
	::Aliro::CryptoTypes::PublicKey key{};
	key[0] = 0x04;
	for (size_t i = 1; i < key.size(); ++i) {
		key[i] = 0x11;
	}
	return key;
}

/** @brief Provisions one credential with a single binding and the given policy; returns its handle. */
::Aliro::UserDevice::CredentialHandle SeedCredential(::Aliro::UserDevice::AuthenticationPolicy policy,
						     const ::Aliro::UserDevice::ReaderGroupIdentifier &readerGroupId)
{
	AliroUd::Credential::Provisioning::Payload payload{};
	payload.mHasNewKeyInput = true;
	payload.mNewKeyScalar = KeyScalar();
	payload.mPolicySet = true;
	payload.mPolicy = policy;
	payload.mBindingCount = 1;
	payload.mBindings[0].mReaderGroupIdentifier = readerGroupId;
	payload.mBindings[0].mTrustType = AliroUd::Credential::TrustType::Direct;
	payload.mBindings[0].mKey = ArbitraryTrustAnchorKey();

	::Aliro::UserDevice::CredentialHandle handle{ ::Aliro::UserDevice::kInvalidCredentialHandle };
	const auto error = AliroUd::Credential::Store::Create(payload, handle);
	zassert_equal(ALIRO_NO_ERROR, error, "Seeding the credential must succeed");
	return handle;
}

const std::vector<uint8_t> kExpeditedPhaseAid{ 0xA0, 0x00, 0x00, 0x09, 0x09, 0xAC, 0xCE, 0x55, 0x01 };

std::vector<uint8_t> SelectCommand()
{
	std::vector<uint8_t> cmd{ 0x00, 0xA4, 0x04, 0x00, static_cast<uint8_t>(kExpeditedPhaseAid.size()) };
	cmd.insert(cmd.end(), kExpeditedPhaseAid.begin(), kExpeditedPhaseAid.end());
	return cmd;
}

void AppendTlv(std::vector<uint8_t> &out, uint8_t tag, const std::vector<uint8_t> &value)
{
	out.push_back(tag);
	out.push_back(static_cast<uint8_t>(value.size()));
	out.insert(out.end(), value.begin(), value.end());
}

std::vector<uint8_t> RepeatedBytes(uint8_t value, size_t count)
{
	return std::vector<uint8_t>(count, value);
}

/**
 * @brief Well-formed AUTH0 command data field (Aliro spec Table 8-4) for
 * `authenticationPolicy`, whose 16-byte reader_group_identifier half of tag
 * 0x4D (`reader_identifier`) is filled with `readerGroupFill`.
 */
std::vector<uint8_t> Auth0Data(uint8_t authenticationPolicy, uint8_t readerGroupFill)
{
	std::vector<uint8_t> data{};
	AppendTlv(data, 0x41, { 0x00 });
	AppendTlv(data, 0x42, { authenticationPolicy });
	AppendTlv(data, 0x5C, { 0x01, 0x00 });
	auto publicKey = RepeatedBytes(0xAA, 65);
	publicKey[0] = 0x04;
	AppendTlv(data, 0x87, publicKey);
	AppendTlv(data, 0x4C, RepeatedBytes(0xBB, 16));
	auto readerIdentifier = RepeatedBytes(readerGroupFill, 16);
	const auto subIdentifier = RepeatedBytes(0xCD, 16);
	readerIdentifier.insert(readerIdentifier.end(), subIdentifier.begin(), subIdentifier.end());
	AppendTlv(data, 0x4D, readerIdentifier);
	return data;
}

std::vector<uint8_t> Auth0Command(uint8_t authenticationPolicy, uint8_t readerGroupFill)
{
	const auto data = Auth0Data(authenticationPolicy, readerGroupFill);
	std::vector<uint8_t> cmd{ 0x80, 0x80, 0x00, 0x00, static_cast<uint8_t>(data.size()) };
	cmd.insert(cmd.end(), data.begin(), data.end());
	return cmd;
}

} // namespace

ZTEST_SUITE(aliro_ud_authorization_e2e, nullptr, SetupSuite, ResetBeforeEachTest, nullptr, nullptr);

/**
 * @brief ALIRO-UD-SYRS-P1-012/020/021: AUTH0 with authentication_policy
 * 0x03 against a matched credential, with no authorization window open,
 * defers exactly one NotifyAuthenticationRequired() call, which turns the
 * visible LED indication on.
 */
ZTEST(aliro_ud_authorization_e2e, test_auth0_policy_0x03_with_no_window_indicates_required)
{
	::Aliro::UserDevice::ReaderGroupIdentifier readerGroupId{};
	readerGroupId.fill(0xAB);
	SeedCredential(::Aliro::UserDevice::AuthenticationPolicy::ForceUserAuthentication, readerGroupId);

	PostFieldOn();
	SettleWorker();
	const auto selectCommand = SelectCommand();
	zassert_equal(0, PostCommandApdu(selectCommand.data(), selectCommand.size()));
	SettleWorker();

	const auto auth0Command = Auth0Command(0x03, 0xAB);
	zassert_equal(0, PostCommandApdu(auth0Command.data(), auth0Command.size()));
	SettleWorker();

	zassert_equal(1u, AliroUd::Authorization::Test::GetSetActiveCallCount(),
		      "Expected exactly one indicator activation for the deferred authentication-required event");
	zassert_true(AliroUd::Authorization::Test::GetLastActive(), "Expected the indicator to be turned on");

	PostFieldOff();
	SettleWorker();
}

/**
 * @brief The same AUTH0 command, after the button opened the authorization
 * window first (`Window::Open()`, standing in for a physical press),
 * proceeds without ever calling NotifyAuthenticationRequired() - the
 * application-owned half of "allow a new transaction to succeed after a
 * button press opens the window" (APP_PLAN.md AWP4).
 */
ZTEST(aliro_ud_authorization_e2e, test_auth0_policy_0x03_with_open_window_does_not_indicate)
{
	::Aliro::UserDevice::ReaderGroupIdentifier readerGroupId{};
	readerGroupId.fill(0xAC);
	SeedCredential(::Aliro::UserDevice::AuthenticationPolicy::ForceUserAuthentication, readerGroupId);

	AliroUd::Authorization::GlobalWindow().Open(k_uptime_get(), 30000);

	PostFieldOn();
	SettleWorker();
	const auto selectCommand = SelectCommand();
	zassert_equal(0, PostCommandApdu(selectCommand.data(), selectCommand.size()));
	SettleWorker();

	const auto auth0Command = Auth0Command(0x03, 0xAC);
	zassert_equal(0, PostCommandApdu(auth0Command.data(), auth0Command.size()));
	SettleWorker();

	zassert_equal(0u, AliroUd::Authorization::Test::GetSetActiveCallCount(),
		      "An open window must not defer/trigger an authentication-required indication");

	PostFieldOff();
	SettleWorker();
}

/**
 * @brief ALIRO-UD-SYRS-P1-031: the AUTH0 response bytes are identical
 * whether or not the authorization window is open - only the deferred
 * NotifyAuthenticationRequired() delivery (asserted above) differs.
 */
ZTEST(aliro_ud_authorization_e2e, test_auth0_response_is_independent_of_window_state)
{
	::Aliro::UserDevice::ReaderGroupIdentifier readerGroupId{};
	readerGroupId.fill(0xAD);
	SeedCredential(::Aliro::UserDevice::AuthenticationPolicy::ForceUserAuthentication, readerGroupId);

	PostFieldOn();
	SettleWorker();
	auto selectCommand = SelectCommand();
	zassert_equal(0, PostCommandApdu(selectCommand.data(), selectCommand.size()));
	SettleWorker();

	auto auth0Command = Auth0Command(0x03, 0xAD);
	zassert_equal(0, PostCommandApdu(auth0Command.data(), auth0Command.size()));
	SettleWorker();
	const auto responseWithoutWindow = AliroUdTest::FakeNfc::GetLastResponse();

	PostFieldOff();
	SettleWorker();

	AliroUd::Authorization::GlobalWindow().Open(k_uptime_get(), 30000);

	PostFieldOn();
	SettleWorker();
	selectCommand = SelectCommand();
	zassert_equal(0, PostCommandApdu(selectCommand.data(), selectCommand.size()));
	SettleWorker();

	auth0Command = Auth0Command(0x03, 0xAD);
	zassert_equal(0, PostCommandApdu(auth0Command.data(), auth0Command.size()));
	SettleWorker();
	const auto responseWithWindow = AliroUdTest::FakeNfc::GetLastResponse();

	zassert_equal(responseWithoutWindow.size(), responseWithWindow.size(),
		      "AUTH0 response length must not depend on window state");
	zassert_mem_equal(responseWithoutWindow.data(), responseWithWindow.data(), responseWithoutWindow.size(),
			   "AUTH0 response bytes must not depend on window state");

	PostFieldOff();
	SettleWorker();
}
