/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#pragma once

/*
 * System OFF support (docs/system_off_proposal.md): Button 0 (`sw0`) is the
 * sole wake source - it wakes the DK from System OFF and opens the
 * authorization window in one press ("wake + authorize"). The NFC frontend
 * is deactivated before every sys_poweroff() so an NFC field cannot wake the
 * DK. Auto-sleep returns the DK to System OFF once the authorization window
 * expires (or after the NFC idle delay). Button 1 (`sw1`) is an awake-only toggle: the
 * first press holds the DK awake indefinitely (e.g. for provisioning), a
 * second press powers it off. Entirely optional - every call site into this
 * module elsewhere is wrapped in IS_ENABLED(CONFIG_ALIRO_UD_SYSTEM_OFF), and
 * this module's own sources are not even compiled in when that symbol is
 * disabled (src/platform/power/CMakeLists.txt), so disabling it removes all
 * runtime power-off behavior with no other code changes.
 *
 * A power-off decision that becomes due while
 * AliroUd::Lifecycle::RunMutation() has a mutation in flight
 * (BeginMutationGuard()/EndMutationGuard(), below) is deferred, never
 * dropped: it fires the instant the mutation ends, whichever of
 * RequestSleep()/the auto idle-timeout caused it.
 */
namespace AliroUd::Power {

/**
 * @brief Starts System OFF support: turns the awake indicator (`led1`) on,
 * initializes Button 1's toggle GPIO backend, and - when this boot resumed
 * from System OFF - opens the authorization window and arms the matching
 * auto-sleep (the Button 0 "wake + authorize" gesture).
 *
 * Call exactly once at boot, after the NFC transport has started.
 */
void Start();

/** @brief NFC field became present: cancels any pending auto-sleep. Called only from platform/nfc. */
void NotifyFieldOn();

/** @brief NFC field is no longer present: arms the idle-delay auto-sleep deadline. Called only from platform/nfc. */
void NotifyFieldOff();

/**
 * @brief Requests an immediate sleep (System OFF).
 *
 * Not gated by the button-authorization window or by an active NFC
 * session (docs/system_off_proposal.md: a deliberate simplification - the
 * operator is trusted to know what they are doing). If a lifecycle
 * mutation is currently in flight (`BeginMutationGuard()`), the request is
 * deferred and fires the instant `EndMutationGuard()` is called, never
 * silently dropped. Called on the Button 1 "exit hold-awake" press (via
 * `HandleButton1Press()`).
 */
void RequestSleep();

/**
 * @brief Button 1 pressed while awake: toggles hold-awake.
 *
 * First press holds the DK awake indefinitely (suppresses auto-sleep, e.g.
 * for provisioning); a subsequent press leaves hold-awake and powers off.
 * Presses within `CONFIG_ALIRO_UD_SYSTEM_OFF_BUTTON1_SETTLE_MS` of this
 * boot's wake are ignored, so the Button 0 wake press cannot cross-trigger
 * this handler and pin the DK awake. Called only from platform/power's
 * Button 1 backend.
 */
void HandleButton1Press();

/**
 * @brief Marks the start of a lifecycle mutation
 * (`AliroUd::Lifecycle::RunMutation()`): suppresses every power-off
 * decision - Button 1 and auto idle-timeout alike - until
 * `EndMutationGuard()`.
 */
void BeginMutationGuard();

/**
 * @brief Marks the end of a lifecycle mutation. Immediately fires any
 * power-off decision that became due while the guard was active.
 */
void EndMutationGuard();

/** @brief Diagnostics: true while a mutation guard is active. */
bool IsMutationGuardActive();

/** @brief Diagnostics: true while a power-off decision is pending (deferred by an active mutation guard, or simply not yet due). */
bool IsShutdownPending();

} // namespace AliroUd::Power
