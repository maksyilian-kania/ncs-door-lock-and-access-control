/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#pragma once

namespace AliroUd::Nfc {

/**
 * @brief Starts the NFC/stack worker thread and the NFC-A Type 4 Tag
 * (ISO-DEP) listen-mode transport.
 *
 * Must be called once at boot, after `Aliro::UserDeviceStack::Instance().Init()`.
 *
 * @return 0 on success, a negative errno value otherwise.
 */
int Start();

/**
 * @brief Deactivates the NFC frontend (`nfc_t4t_emulation_stop()`).
 *
 * Called immediately before `sys_poweroff()` (System OFF) so an NFC field
 * can no longer wake the DK: Button 0 is the sole wake source. A live NFC
 * session is RAM-only and reinitializes cleanly on the next boot, so
 * stopping mid-transaction is equivalent to the field being pulled away.
 * No-op-safe to call even if emulation was never started.
 */
void StopEmulation();

} // namespace AliroUd::Nfc
