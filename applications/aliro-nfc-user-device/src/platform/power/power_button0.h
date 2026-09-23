/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#pragma once

/*
 * Button 0 (`sw0`) System OFF wake backend (docs/system_off_proposal.md).
 *
 * `sw0`'s normal (awake) role - opening the authorization window on an edge
 * press - is owned by authorization_button.cpp and is unchanged. This module
 * only reconfigures the same pin to a level-sense ("SENSE") interrupt
 * immediately before `sys_poweroff()` so a later press wakes the DK from
 * System OFF. Button 0 is the sole wake source: Button 1 (`sw1`) is an
 * awake-only hold/off toggle and is not armed for wake, and the NFC frontend
 * is deactivated before power-off so an NFC field cannot wake the DK either.
 */
namespace AliroUd::Power::Button0 {

/**
 * @brief Reconfigures `sw0` to a level-sense wake interrupt.
 *
 * Must be called immediately before every `sys_poweroff()` call. A no-op
 * (with a one-time warning) if no `sw0` devicetree alias exists.
 */
void ConfigureWakeSense();

} // namespace AliroUd::Power::Button0
