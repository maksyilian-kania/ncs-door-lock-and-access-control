/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#pragma once

/*
 * Visible authorization indication (APP_PLAN.md AWP4: "Show an LED
 * indication when authorization is required and no valid window exists").
 *
 * Split out from authorization.cpp so host tests can substitute a fake
 * recorder for the real DK LED GPIO (authorization_led.cpp), the same
 * pattern used for Aliro::Interface::UserDevice::Nfc/nfc_transport.cpp.
 */
namespace AliroUd::Authorization::Indicator {

/**
 * @brief Sets the visible "authorization required" indication on/off.
 *
 * @param active true to turn the indication on (authorization is required
 * and no valid window exists), false to turn it off.
 */
void SetActive(bool active);

} // namespace AliroUd::Authorization::Indicator
