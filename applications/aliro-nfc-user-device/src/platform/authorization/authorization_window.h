/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#pragma once

#include <cstdint>

#include <zephyr/spinlock.h>

#include <aliro/user_device/types.h>

/*
 * One device-global button-authorization window (APP_PLAN.md AWP4,
 * ALIRO-UD-SYRS-P1-011/012/020/021).
 *
 * The public Aliro::Interface::UserDevice::Authorization::GetState()
 * contract carries only a CredentialHandle, and a physical DK button press
 * represents "the device operator is physically present" independent of
 * which Access Credential a Reader eventually selects; every credential
 * therefore shares this one window rather than each tracking its own.
 *
 * Every timestamp is an explicit caller-supplied millisecond value (not an
 * internal k_uptime_get() call) so the window state machine itself has no
 * clock dependency and can be driven by a fake monotonic clock in host
 * tests (APP_PLAN.md AWP4 Verify: "with a fake monotonic clock").
 */
namespace AliroUd::Authorization {

class Window {
public:
	/**
	 * @brief Opens (or re-opens/extends) the window: valid from `nowMs`
	 * until `nowMs + durationMs`, exclusive of the deadline itself.
	 *
	 * Safe to call repeatedly (a later call always replaces any earlier
	 * deadline, whether still valid or already expired), so repeated
	 * button presses simply restart the window.
	 */
	void Open(int64_t nowMs, uint32_t durationMs);

	/** @brief Immediately revokes any open window, regardless of `nowMs`. */
	void Close();

	/** @brief True while a window opened by `Open()` has not yet reached its deadline at `nowMs`. */
	bool IsValid(int64_t nowMs) const;

	/**
	 * @brief `AuthorizationState::Authorized` while `IsValid(nowMs)`,
	 * `AuthorizationState::Required` otherwise. Never returns `Denied`:
	 * this window has no concept of an explicit denial, only "currently
	 * open" or "not (yet) open".
	 */
	::Aliro::UserDevice::AuthorizationState GetState(int64_t nowMs) const;

	/** @brief Milliseconds remaining until expiry at `nowMs`, or 0 if no valid window exists. */
	int64_t GetRemainingMs(int64_t nowMs) const;

private:
	mutable struct k_spinlock mLock {};
	bool mHasDeadline{ false };
	int64_t mDeadlineMs{ 0 };
};

/**
 * @brief The single window shared by every credential handle.
 *
 * `AliroUd::Cli`, the DK button ISR (`authorization_button.cpp`), and the
 * `Aliro::Interface::UserDevice::Authorization` adapter (`authorization.cpp`)
 * all read/write this one instance.
 */
Window &GlobalWindow();

} // namespace AliroUd::Authorization
