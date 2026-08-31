/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include <zephyr/kernel.h>
#include <zephyr/ztest.h>

#include "fake_authorization_indicator.h"
#include "platform/authorization/authorization_window.h"

#include <aliro/user_device/interface.h>

/*
 * Contract-level tests for Aliro::Interface::UserDevice::Authorization
 * (APP_PLAN.md AWP4): the real authorization.cpp adapter in front of the
 * real AliroUd::Authorization::GlobalWindow() and a fake LED indicator
 * recorder (native_sim has no `led0` devicetree alias).
 */

using AuthorizationState = ::Aliro::UserDevice::AuthorizationState;

namespace {

void ResetBeforeEachTest(void *fixture)
{
	(void)fixture;
	AliroUd::Authorization::GlobalWindow().Close();
	AliroUd::Authorization::Test::ResetFakeAuthorizationIndicator();
}

} // namespace

ZTEST_SUITE(aliro_ud_authorization_contract, nullptr, nullptr, ResetBeforeEachTest, nullptr, nullptr);

/** @brief With no window open, GetState() reports Required for any credential handle. */
ZTEST(aliro_ud_authorization_contract, test_get_state_required_with_no_window)
{
	AuthorizationState state{ AuthorizationState::Authorized };

	const auto error = Aliro::Interface::UserDevice::Authorization::GetState(1, state);

	zassert_equal(ALIRO_NO_ERROR, error, "GetState() must succeed");
	zassert_equal(static_cast<int>(AuthorizationState::Required), static_cast<int>(state),
		      "Expected Required with no open window");
}

/** @brief GetState() reflects a window opened directly on AliroUd::Authorization::GlobalWindow() (button press). */
ZTEST(aliro_ud_authorization_contract, test_get_state_authorized_after_window_opened)
{
	AliroUd::Authorization::GlobalWindow().Open(k_uptime_get(), 30000);

	AuthorizationState state{ AuthorizationState::Required };
	const auto error = Aliro::Interface::UserDevice::Authorization::GetState(1, state);

	zassert_equal(ALIRO_NO_ERROR, error, "GetState() must succeed");
	zassert_equal(static_cast<int>(AuthorizationState::Authorized), static_cast<int>(state),
		      "Expected Authorized with an open window");
}

/** @brief GetState() ignores the credential handle: the window is device-global, not per-credential. */
ZTEST(aliro_ud_authorization_contract, test_get_state_is_credential_independent)
{
	AliroUd::Authorization::GlobalWindow().Open(k_uptime_get(), 30000);

	AuthorizationState stateForOne{ AuthorizationState::Required };
	AuthorizationState stateForOther{ AuthorizationState::Required };
	zassert_equal(ALIRO_NO_ERROR, Aliro::Interface::UserDevice::Authorization::GetState(1, stateForOne));
	zassert_equal(ALIRO_NO_ERROR,
		      Aliro::Interface::UserDevice::Authorization::GetState(0xDEADBEEF, stateForOther));

	zassert_equal(static_cast<int>(AuthorizationState::Authorized), static_cast<int>(stateForOne),
		      "Handle 1 must see the shared window");
	zassert_equal(static_cast<int>(AuthorizationState::Authorized), static_cast<int>(stateForOther),
		      "An arbitrary other handle must see the same shared window");
}

/** @brief NotifyAuthenticationRequired() turns the visible indication on exactly once per call. */
ZTEST(aliro_ud_authorization_contract, test_notify_authentication_required_activates_indicator)
{
	zassert_equal(0u, AliroUd::Authorization::Test::GetSetActiveCallCount(), "No indicator call yet");

	Aliro::Interface::UserDevice::Authorization::NotifyAuthenticationRequired(1);

	zassert_equal(1u, AliroUd::Authorization::Test::GetSetActiveCallCount(),
		      "Expected exactly one indicator call");
	zassert_true(AliroUd::Authorization::Test::GetLastActive(), "Expected the indicator to be turned on");
}
