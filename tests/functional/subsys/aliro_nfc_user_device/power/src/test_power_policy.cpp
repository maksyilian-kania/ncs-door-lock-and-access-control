/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include <zephyr/ztest.h>

#include "platform/power/power_policy.h"

/*
 * Pure logic tests for AliroUd::Power::Policy (docs/system_off_proposal.md).
 * Every timestamp is an explicit constant passed to the API under test - no
 * dependency on k_uptime_get() - mirroring
 * authorization/src/test_authorization_window.cpp's "fake monotonic clock"
 * approach.
 */

using AliroUd::Power::Policy;

ZTEST_SUITE(aliro_ud_power_policy, nullptr, nullptr, nullptr, nullptr, nullptr);

/** @brief A policy with no trigger armed never reports a power-off, at any time. */
ZTEST(aliro_ud_power_policy, test_fresh_policy_never_powers_off)
{
	Policy policy{};

	zassert_false(policy.ShouldPowerOff(0, false), "Fresh policy must not request power-off");
	zassert_false(policy.ShouldPowerOff(1000000, false), "Fresh policy must not request power-off later either");
	zassert_false(policy.IsShutdownPending(), "Fresh policy must have nothing pending");
	zassert_false(policy.IsMutationGuardActive(), "Fresh policy must have no active mutation guard");
}

/** @brief The auto idle-timeout trigger stays false strictly before its deadline, true at/after it (window closed). */
ZTEST(aliro_ud_power_policy, test_idle_timeout_fires_at_deadline)
{
	Policy policy{};

	policy.NotifyFieldOff(0, 5000);

	zassert_false(policy.ShouldPowerOff(4999, false), "Must not fire 1 ms before the idle deadline");
	zassert_true(policy.ShouldPowerOff(5000, false), "Must fire exactly at the idle deadline");
}

/** @brief Once fired, the idle-timeout trigger is consumed and does not fire again without a fresh NotifyFieldOff(). */
ZTEST(aliro_ud_power_policy, test_idle_timeout_is_consumed_after_firing)
{
	Policy policy{};

	policy.NotifyFieldOff(0, 1000);
	zassert_true(policy.ShouldPowerOff(1000, false), "Expected the deadline to fire");
	zassert_false(policy.ShouldPowerOff(2000, false), "Must not fire a second time without a fresh NotifyFieldOff()");
	zassert_false(policy.IsShutdownPending(), "Nothing should be pending after the trigger fired");
}

/** @brief A field-on cancels an armed idle-timeout deadline; it never fires even after the original deadline passes. */
ZTEST(aliro_ud_power_policy, test_field_on_cancels_idle_timeout)
{
	Policy policy{};

	policy.NotifyFieldOff(0, 1000);
	policy.NotifyFieldOn();

	zassert_false(policy.ShouldPowerOff(1000, false), "FIELD_ON must cancel the pending idle deadline");
	zassert_false(policy.ShouldPowerOff(1000000, false), "Cancelled deadline must never fire later either");
}

/** @brief docs/system_off_proposal.md: "the 30 s window must never be interrupted by an automatic power-off." */
ZTEST(aliro_ud_power_policy, test_valid_window_suppresses_idle_timeout)
{
	Policy policy{};

	policy.NotifyFieldOff(0, 1000);

	zassert_false(policy.ShouldPowerOff(1000, true), "A valid window must suppress the auto idle-timeout");
	zassert_false(policy.ShouldPowerOff(5000, true), "Suppression must hold for as long as the window stays valid");
}

/** @brief Once the window expires, the still-armed idle-timeout trigger fires on the next check, with no fresh NotifyFieldOff(). */
ZTEST(aliro_ud_power_policy, test_idle_timeout_fires_once_window_expires)
{
	Policy policy{};

	policy.NotifyFieldOff(0, 1000);
	zassert_false(policy.ShouldPowerOff(1000, true), "Suppressed while the window is valid");

	zassert_true(policy.ShouldPowerOff(2000, false), "Must fire once the window is no longer valid");
}

/** @brief Button 1's manual sleep fires immediately, even before any idle deadline would have been reached. */
ZTEST(aliro_ud_power_policy, test_manual_sleep_fires_immediately)
{
	Policy policy{};

	policy.RequestManualSleep();

	zassert_true(policy.ShouldPowerOff(0, false), "Manual sleep must fire on the very next check");
}

/** @brief docs/system_off_proposal.md: "Button 1's explicit press is NOT gated by the window." */
ZTEST(aliro_ud_power_policy, test_manual_sleep_is_not_gated_by_window)
{
	Policy policy{};

	policy.RequestManualSleep();

	zassert_true(policy.ShouldPowerOff(0, true), "Manual sleep must fire regardless of window validity");
}

/** @brief A manual sleep request is consumed after firing once. */
ZTEST(aliro_ud_power_policy, test_manual_sleep_is_consumed_after_firing)
{
	Policy policy{};

	policy.RequestManualSleep();
	zassert_true(policy.ShouldPowerOff(0, false));
	zassert_false(policy.ShouldPowerOff(1, false), "Must not fire a second time without a fresh RequestManualSleep()");
}

/**
 * @brief The core "deferred, never dropped" requirement: a manual sleep
 * requested while a mutation guard is active must not fire, but must fire
 * the instant the guard is released - not require re-pressing the button.
 */
ZTEST(aliro_ud_power_policy, test_manual_sleep_deferred_by_mutation_guard_then_fires_on_release)
{
	Policy policy{};

	policy.BeginMutationGuard();
	policy.RequestManualSleep();

	zassert_false(policy.ShouldPowerOff(0, false), "Must not power off while the mutation guard is active");
	zassert_true(policy.IsShutdownPending(), "The request must not be dropped: it stays pending");

	policy.EndMutationGuard();

	zassert_true(policy.ShouldPowerOff(0, false), "Must fire immediately once the mutation guard is released");
}

/** @brief The same deferred-not-dropped guarantee applies to the auto idle-timeout trigger, not only the manual one. */
ZTEST(aliro_ud_power_policy, test_idle_timeout_deferred_by_mutation_guard_then_fires_on_release)
{
	Policy policy{};

	policy.NotifyFieldOff(0, 1000);
	policy.BeginMutationGuard();

	zassert_false(policy.ShouldPowerOff(1000, false), "Must not power off while the mutation guard is active");

	policy.EndMutationGuard();

	zassert_true(policy.ShouldPowerOff(1000, false), "Must fire once the mutation guard is released");
}

/** @brief Nested mutation guards (RunMutation() calls are documented as non-reentrant, but the depth counter is defensive): every Begin needs a matching End. */
ZTEST(aliro_ud_power_policy, test_mutation_guard_is_depth_counted)
{
	Policy policy{};

	policy.BeginMutationGuard();
	policy.BeginMutationGuard();
	zassert_true(policy.IsMutationGuardActive());

	policy.EndMutationGuard();
	zassert_true(policy.IsMutationGuardActive(), "One matching End() must not release a guard entered twice");

	policy.EndMutationGuard();
	zassert_false(policy.IsMutationGuardActive(), "The second matching End() must release the guard");
}

/** @brief An extra EndMutationGuard() with no matching Begin() must not underflow into a false "active" state or crash. */
ZTEST(aliro_ud_power_policy, test_unmatched_end_mutation_guard_is_safe)
{
	Policy policy{};

	policy.EndMutationGuard();

	zassert_false(policy.IsMutationGuardActive());
	policy.RequestManualSleep();
	zassert_true(policy.ShouldPowerOff(0, false), "An unmatched End() must not leave the guard permanently active");
}

/** @brief While a mutation guard is active, neither trigger fires even if both are armed simultaneously. */
ZTEST(aliro_ud_power_policy, test_mutation_guard_blocks_both_triggers_at_once)
{
	Policy policy{};

	policy.NotifyFieldOff(0, 1000);
	policy.RequestManualSleep();
	policy.BeginMutationGuard();

	zassert_false(policy.ShouldPowerOff(1000, false), "Neither trigger must fire while the guard is active");

	policy.EndMutationGuard();

	zassert_true(policy.ShouldPowerOff(1000, false), "One of the two pending triggers must fire once released");
	/* Manual sleep is checked first and consumes the idle deadline too (one power-off is enough). */
	zassert_false(policy.IsShutdownPending(), "Both triggers should be consumed by the single power-off decision");
}

/** @brief IsShutdownPending() reflects an armed-but-not-yet-due idle deadline too, not only an overdue one. */
ZTEST(aliro_ud_power_policy, test_is_shutdown_pending_reflects_armed_not_yet_due_deadline)
{
	Policy policy{};

	policy.NotifyFieldOff(0, 5000);
	zassert_true(policy.IsShutdownPending(), "An armed idle deadline must report as pending even before it is due");
	zassert_false(policy.ShouldPowerOff(0, false), "But must not fire before its deadline");
}

/** @brief ArmAutoSleep() arms the same auto idle-timeout as NotifyFieldOff() (the boot-from-System-OFF path). */
ZTEST(aliro_ud_power_policy, test_arm_auto_sleep_fires_at_deadline)
{
	Policy policy{};

	policy.ArmAutoSleep(0, 30000);

	zassert_false(policy.ShouldPowerOff(29999, false), "Must not fire before the boot auto-sleep deadline");
	zassert_true(policy.ShouldPowerOff(30000, false), "Must fire at the boot auto-sleep deadline");
}

/** @brief Hold-awake (Button 1 first press) suppresses the auto idle-timeout indefinitely. */
ZTEST(aliro_ud_power_policy, test_hold_awake_suppresses_idle_timeout)
{
	Policy policy{};

	policy.ArmAutoSleep(0, 1000);
	policy.SetHoldAwake();

	zassert_true(policy.IsHoldAwake(), "Hold-awake must report as active");
	zassert_false(policy.ShouldPowerOff(1000, false), "Hold-awake must suppress the auto idle-timeout");
	zassert_false(policy.ShouldPowerOff(1000000, false), "Suppression is indefinite");
}

/** @brief Hold-awake also stops NFC field-off from re-arming an auto-sleep. */
ZTEST(aliro_ud_power_policy, test_hold_awake_blocks_field_off_rearm)
{
	Policy policy{};

	policy.SetHoldAwake();
	policy.NotifyFieldOff(0, 1000);

	zassert_false(policy.IsShutdownPending(), "Field-off must not arm auto-sleep while hold-awake");
	zassert_false(policy.ShouldPowerOff(1000, false), "And nothing must fire");
}

/** @brief The manual (Button 1 exit) sleep still fires while hold-awake is active. */
ZTEST(aliro_ud_power_policy, test_manual_sleep_fires_despite_hold_awake)
{
	Policy policy{};

	policy.SetHoldAwake();
	policy.RequestManualSleep();

	zassert_true(policy.ShouldPowerOff(0, false), "Manual sleep must fire even while hold-awake");
}

/** @brief Clearing hold-awake re-enables the auto idle-timeout. */
ZTEST(aliro_ud_power_policy, test_clear_hold_awake_reenables_idle_timeout)
{
	Policy policy{};

	policy.SetHoldAwake();
	policy.ClearHoldAwake();
	zassert_false(policy.IsHoldAwake(), "Hold-awake must report as cleared");

	policy.NotifyFieldOff(0, 1000);
	zassert_true(policy.ShouldPowerOff(1000, false), "Auto idle-timeout must work again once hold-awake is cleared");
}
