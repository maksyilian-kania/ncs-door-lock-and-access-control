/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include <zephyr/ztest.h>

#include "platform/authorization/authorization_window.h"

/*
 * Pure logic tests for AliroUd::Authorization::Window (APP_PLAN.md AWP4).
 * Every timestamp is an explicit constant passed to the API under test, so
 * this exercises the "fake monotonic clock" Verify requirement without any
 * dependency on wall-clock/k_uptime_get() timing.
 */

using AliroUd::Authorization::Window;
using AuthorizationState = ::Aliro::UserDevice::AuthorizationState;

ZTEST_SUITE(aliro_ud_authorization_window, nullptr, nullptr, nullptr, nullptr, nullptr);

/** @brief ALIRO-UD-SYRS-P1-012: a window that was never opened reports Required, not Authorized or Denied. */
ZTEST(aliro_ud_authorization_window, test_never_opened_reports_required)
{
	Window window{};

	zassert_equal(static_cast<int>(AuthorizationState::Required), static_cast<int>(window.GetState(0)),
		      "Fresh window must fail closed (Required)");
	zassert_false(window.IsValid(0), "Fresh window must not be valid");
	zassert_equal(0, window.GetRemainingMs(0), "Fresh window must report zero remaining time");
}

/** @brief ALIRO-UD-SYRS-P1-011: a button press (Open()) makes a later GetState() query report Authorized. */
ZTEST(aliro_ud_authorization_window, test_preauthorization_before_transaction)
{
	Window window{};

	window.Open(0, 30000);

	zassert_equal(static_cast<int>(AuthorizationState::Authorized), static_cast<int>(window.GetState(5000)),
		      "A transaction well inside the window must see Authorized");
}

/** @brief The window is valid strictly before its deadline and expires exactly at it. */
ZTEST(aliro_ud_authorization_window, test_window_expires_at_deadline)
{
	Window window{};

	window.Open(0, 1000);

	zassert_true(window.IsValid(999), "Window must still be valid 1 ms before the deadline");
	zassert_false(window.IsValid(1000), "Window must be expired exactly at the deadline");
	zassert_equal(static_cast<int>(AuthorizationState::Required), static_cast<int>(window.GetState(1000)),
		      "Expired window must report Required");
}

/** @brief 1-second boundary (Kconfig minimum ALIRO_UD_AUTHORIZATION_WINDOW_SECONDS). */
ZTEST(aliro_ud_authorization_window, test_one_second_boundary)
{
	Window window{};

	window.Open(1000, 1000);

	zassert_true(window.IsValid(1999), "1 s window must still be valid 1 ms before its deadline");
	zassert_false(window.IsValid(2000), "1 s window must be expired exactly at its deadline");
}

/** @brief 300-second boundary (Kconfig maximum ALIRO_UD_AUTHORIZATION_WINDOW_SECONDS). */
ZTEST(aliro_ud_authorization_window, test_three_hundred_second_boundary)
{
	Window window{};

	window.Open(0, 300000);

	zassert_true(window.IsValid(299999), "300 s window must still be valid 1 ms before its deadline");
	zassert_false(window.IsValid(300000), "300 s window must be expired exactly at its deadline");
}

/** @brief A repeated press before expiry restarts (extends) the window rather than being ignored. */
ZTEST(aliro_ud_authorization_window, test_repeated_press_extends_window)
{
	Window window{};

	window.Open(0, 1000);
	zassert_true(window.IsValid(900), "Window must be valid before the repeated press");

	window.Open(900, 1000);

	zassert_true(window.IsValid(1500), "Repeated press must extend validity past the original deadline");
	zassert_false(window.IsValid(1900), "Extended window must still expire at its own new deadline");
}

/** @brief A repeated press after the window already expired (retry) opens a fresh window. */
ZTEST(aliro_ud_authorization_window, test_retry_after_expiry_reopens_window)
{
	Window window{};

	window.Open(0, 1000);
	zassert_false(window.IsValid(2000), "Window must have expired by t=2000");

	window.Open(2000, 1000);

	zassert_true(window.IsValid(2999), "Retried press must open a fresh, valid window");
	zassert_false(window.IsValid(3000), "Retried window must still expire at its own deadline");
}

/** @brief Close() immediately revokes an open window, independent of `nowMs` (explicit denial/reset). */
ZTEST(aliro_ud_authorization_window, test_close_immediately_revokes)
{
	Window window{};

	window.Open(0, 30000);
	zassert_true(window.IsValid(0), "Window must be valid immediately after opening");

	window.Close();

	zassert_false(window.IsValid(0), "Window must be revoked immediately after Close()");
	zassert_equal(static_cast<int>(AuthorizationState::Required), static_cast<int>(window.GetState(0)),
		      "Closed window must report Required");
}

/** @brief GetRemainingMs() reports a decreasing, never-negative countdown and 0 once expired. */
ZTEST(aliro_ud_authorization_window, test_get_remaining_ms)
{
	Window window{};

	window.Open(0, 1000);

	zassert_equal(1000, window.GetRemainingMs(0), "Remaining time must equal the full duration at open");
	zassert_equal(400, window.GetRemainingMs(600), "Remaining time must count down");
	zassert_equal(0, window.GetRemainingMs(1000), "Remaining time must be 0 at the deadline");
	zassert_equal(0, window.GetRemainingMs(5000), "Remaining time must be 0, never negative, after expiry");
}

/** @brief GlobalWindow() always returns the same shared instance. */
ZTEST(aliro_ud_authorization_window, test_global_window_is_a_singleton)
{
	zassert_equal(&AliroUd::Authorization::GlobalWindow(), &AliroUd::Authorization::GlobalWindow(),
		      "GlobalWindow() must return the same instance every time");
}
