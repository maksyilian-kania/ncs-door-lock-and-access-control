/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#pragma once

#include <cstddef>
#include <cstdint>

/*
 * The bounded queue/coalesced field channel and dedicated high-priority
 * thread that serializes NFC transport events and deferred Aliro User
 * Device stack events into calls on Aliro::UserDeviceStack (APP_PLAN.md
 * AWP1). nfc_t4t_lib callbacks (nfc_transport.cpp) and
 * Aliro::Interface::UserDevice::Os::QueueEvent() (os_queue.cpp) only ever
 * reach the stack through this module; neither calls stack, storage,
 * shell, or cryptographic operations directly.
 *
 * Every mutation of the application's local "is a session active" belief
 * happens exclusively on the dedicated worker thread: FIELD_ON/FIELD_OFF
 * are only ever applied there, and `NotifySessionTerminated()` (called by
 * `Aliro::Interface::UserDevice::Nfc::HandleTermination()`, itself only
 * ever invoked synchronously from a call already running on this thread)
 * is the single place that clears it when the stack ends a session
 * independently (watchdog timeout, protocol failure).
 */
namespace AliroUd::Nfc {

/** @brief Maximum payload length of one transport-assembled command or response APDU (ISO/IEC 7816-4 short APDU). */
constexpr size_t kMaxApduLength{ 261 };

/**
 * @brief Starts the dedicated NFC/stack worker thread.
 *
 * Must be called exactly once at boot, before any event is posted.
 */
void StartWorker();

/**
 * @brief Posts an "NFC field activated" event.
 *
 * Safe to call from the nfc_t4t_lib callback context. Never fails: the
 * pending field intent is coalesced (latest wins) rather than queued, so a
 * FIELD_ON can never be dropped for lack of queue space. The worker thread
 * calls `Aliro::UserDeviceStack::ActivateSession()` at most once per
 * inactive-to-active transition; a FIELD_ON observed while already active
 * is logged and ignored without a second activation attempt.
 */
void PostFieldOn();

/**
 * @brief Posts an "NFC field deactivated" event.
 *
 * Safe to call from the nfc_t4t_lib callback context. Never fails, for the
 * same reason as `PostFieldOn()`. A FIELD_OFF observed while already
 * inactive is logged and ignored without calling
 * `Aliro::UserDeviceStack::DeactivateSession()` again.
 */
void PostFieldOff();

/**
 * @brief Posts one complete, transport-assembled command APDU.
 *
 * @param apdu Command APDU bytes; copied into the queue entry, so the
 * caller's buffer may be reused immediately after this call returns.
 * @param length Number of bytes in `apdu`; must not exceed `kMaxApduLength`.
 *
 * @return 0 on success, a negative errno value otherwise (`-EINVAL` if
 * `length` exceeds `kMaxApduLength`, `-ENOMSG`/queue-full errno otherwise).
 * A queue-full failure here forces deterministic session recovery (see
 * `nfc_worker.cpp`) rather than silently leaving stale protocol state.
 */
int PostCommandApdu(const uint8_t *apdu, size_t length);

/**
 * @brief Posts an opaque stack-owned deferred event.
 *
 * Implements the delivery side of
 * `Aliro::Interface::UserDevice::Os::QueueEvent()`: `event` is later passed,
 * unchanged, only to `Aliro::UserDeviceStack::ProcessEvent()`, on the
 * worker thread. Safe to call from ISR/timer context.
 *
 * @return 0 on success, a negative errno value otherwise. A queue-full
 * failure here forces deterministic session recovery (see
 * `nfc_worker.cpp`) rather than silently dropping a stack event such as a
 * watchdog timeout, which could otherwise leave the application's local
 * session-active belief stale forever.
 */
int PostStackEvent(void *event);

/**
 * @brief Notifies the worker/session tracker that the stack ended the
 * current session on its own (watchdog timeout, protocol failure), as
 * opposed to a FIELD_OFF the worker itself requested.
 *
 * Must be called by `Aliro::Interface::UserDevice::Nfc::HandleTermination()`.
 * Always invoked synchronously on the worker thread (transitively, from
 * `PostFieldOff()`'s own `DeactivateSession()` call, or from stack-event
 * processing), so no additional synchronization is required here.
 */
void NotifySessionTerminated();

/**
 * @brief Prevents NFC field activation and synchronously terminates any
 * currently active session (APP_PLAN.md AWP3 lifecycle coordinator).
 *
 * Blocks the calling thread until the worker thread has processed the
 * request: on return, no session is active and no FIELD_ON observed after
 * this call returns will activate one, until `ExitMaintenancePause()` is
 * called. Must be paired with exactly one `ExitMaintenancePause()` call;
 * nesting is not supported (see `AliroUd::Lifecycle::RunMutation()`, the
 * only intended caller).
 */
void EnterMaintenancePause();

/** @brief Re-allows NFC field activation after `EnterMaintenancePause()`. */
void ExitMaintenancePause();

/** @brief Diagnostics: whether the application currently believes a session is active. */
bool IsSessionActive();

/** @brief Diagnostics: number of `Aliro::UserDeviceStack::ActivateSession()` calls made so far. */
size_t GetActivationAttemptCount();

/** @brief Diagnostics: number of command APDUs dropped so far because no session was believed active. */
size_t GetRejectedApduCount();

} // namespace AliroUd::Nfc
