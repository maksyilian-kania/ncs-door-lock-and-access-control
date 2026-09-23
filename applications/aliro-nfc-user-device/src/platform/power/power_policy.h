/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#pragma once

#include <cstdint>

#include <zephyr/spinlock.h>

/*
 * Pure System OFF decision logic (docs/system_off_proposal.md), decoupled
 * from any Zephyr timer/GPIO/sys_poweroff() call so it is host-testable
 * with an explicit, caller-supplied monotonic clock - the same approach as
 * AliroUd::Authorization::Window. platform/power/power.cpp is the only
 * intended caller; it owns the k_work_delayable idle timer, the Button 1
 * GPIO, the led1 indicator, and the single sys_poweroff() call site that
 * act on this class's decisions.
 */
namespace AliroUd::Power {

/**
 * @brief Combines three independent facts into one power-off decision:
 *  - whether an NFC-field idle-timeout deadline is currently armed (auto-sleep
 *    eligibility),
 *  - whether Button 1 has requested an immediate manual sleep,
 *  - whether a lifecycle mutation (credential/mailbox commit, delete, or
 *    reset) is currently in flight and must not be interrupted.
 *
 * A power-off that becomes due while a mutation is in flight is never
 * dropped: `ShouldPowerOff()` simply keeps returning false while the guard
 * is active, and the caller re-offers the same still-pending trigger the
 * moment `EndMutationGuard()` returns, so a deferred shutdown always fires
 * as soon as - but never before - it is safe to do so.
 */
class Policy {
public:
	/** @brief NFC field is now present: cancels any armed auto-sleep deadline. */
	void NotifyFieldOn();

	/**
	 * @brief NFC field is no longer present: arms an auto-sleep deadline at
	 * `nowMs + idleDelayMs`. A no-op while hold-awake is active (Button 1
	 * has pinned the DK awake), so field activity can never re-arm sleep
	 * once the operator has explicitly asked to stay awake.
	 */
	void NotifyFieldOff(int64_t nowMs, uint32_t idleDelayMs);

	/**
	 * @brief Arms an auto-sleep deadline at `nowMs + delayMs` for the
	 * boot-from-System-OFF path (Button 0 wake opens the authorization
	 * window and this arms the matching auto-sleep for when it expires).
	 * Unlike `NotifyFieldOff()` this is not suppressed by hold-awake, since
	 * it is only ever called once at boot before any Button 1 press.
	 */
	void ArmAutoSleep(int64_t nowMs, uint32_t delayMs);

	/** @brief Button 1 exit-press: requests an unconditional manual sleep, never gated by the authorization window or hold-awake. */
	void RequestManualSleep();

	/**
	 * @brief Enters "hold awake" (Button 1 first press after a Button 0
	 * wake): suppresses the auto idle-timeout indefinitely so the DK stays
	 * reachable (e.g. for provisioning). The manual-sleep trigger still
	 * fires, so a second Button 1 press can power off.
	 */
	void SetHoldAwake();

	/** @brief Leaves "hold awake". */
	void ClearHoldAwake();

	/** @brief Diagnostics: true while hold-awake suppresses the auto idle-timeout. */
	bool IsHoldAwake() const;

	/**
	 * @brief Marks the start of a lifecycle mutation. Nestable (depth-counted):
	 * `ShouldPowerOff()` returns false unconditionally until every
	 * `BeginMutationGuard()` call has a matching `EndMutationGuard()`.
	 */
	void BeginMutationGuard();

	/** @brief Marks the end of one lifecycle mutation. */
	void EndMutationGuard();

	/**
	 * @brief Evaluates every pending trigger against `nowMs`/`windowValid` and
	 * returns whether the caller should call `sys_poweroff()` now.
	 *
	 * On returning true, the triggering fact (manual-sleep request, or the
	 * idle deadline) is consumed and will not fire again until re-armed by
	 * another `RequestManualSleep()`/`NotifyFieldOff()` call.
	 *
	 * @param nowMs Current monotonic time in milliseconds.
	 * @param windowValid Whether `AliroUd::Authorization::Window` currently
	 * holds a valid button-authorization window. Only the auto idle-timeout
	 * trigger is suppressed by this (and by hold-awake); the manual Button 1
	 * exit trigger never is (docs/system_off_proposal.md: "Button 1's
	 * explicit press is NOT gated by the window").
	 */
	bool ShouldPowerOff(int64_t nowMs, bool windowValid);

	/** @brief Diagnostics: true while at least one mutation guard is active. */
	bool IsMutationGuardActive() const;

	/** @brief Diagnostics: true while a power-off trigger is armed/pending (manual request or an idle deadline, whether or not it has been reached yet). */
	bool IsShutdownPending() const;

private:
	mutable struct k_spinlock mLock{};
	bool mManualSleepRequested{ false };
	bool mIdleDeadlineSet{ false };
	int64_t mIdleDeadlineMs{ 0 };
	unsigned mMutationGuardDepth{ 0 };
	bool mHoldAwake{ false };
};

} // namespace AliroUd::Power
