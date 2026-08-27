/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#pragma once

#include <cstddef>
#include <cstdint>

/*
 * The bounded queue and dedicated high-priority thread that serializes NFC
 * transport events and deferred Aliro User Device stack events into calls
 * on Aliro::UserDeviceStack (APP_PLAN.md AWP1). nfc_t4t_lib callbacks
 * (nfc_transport.cpp) and Aliro::Interface::UserDevice::Os::QueueEvent()
 * (os_queue.cpp) only ever reach the stack through this module; neither
 * calls stack, storage, shell, or cryptographic operations directly.
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

/** @brief Posts an "NFC field activated" event. Safe to call from the nfc_t4t_lib callback context. */
int PostFieldOn();

/** @brief Posts an "NFC field deactivated" event. Safe to call from the nfc_t4t_lib callback context. */
int PostFieldOff();

/**
 * @brief Posts one complete, transport-assembled command APDU.
 *
 * @param apdu Command APDU bytes; copied into the queue entry, so the
 * caller's buffer may be reused immediately after this call returns.
 * @param length Number of bytes in `apdu`; must not exceed `kMaxApduLength`.
 *
 * @return 0 on success, a negative errno value otherwise (`-EINVAL` if
 * `length` exceeds `kMaxApduLength`, `-ENOMSG`/queue-full errno otherwise).
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
 * @return 0 on success, a negative errno value otherwise.
 */
int PostStackEvent(void *event);

} // namespace AliroUd::Nfc
