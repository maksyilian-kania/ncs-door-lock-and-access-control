/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#pragma once

/*
 * Button 1 (`sw1`) awake-only hold/off toggle GPIO backend
 * (docs/system_off_proposal.md): an edge-triggered interrupt while the DK is
 * awake whose press is routed to `AliroUd::Power::HandleButton1Press()`
 * (first press holds the DK awake, a second powers off). Button 1 is
 * deliberately NOT a System OFF wake source; Button 0 (`sw0`,
 * authorization_button.cpp) is the sole button wake source.
 */
namespace AliroUd::Power::Button1 {

/**
 * @brief Configures `sw1` as input and installs its press callback.
 *
 * The edge interrupt is armed only after a short settle delay
 * (`CONFIG_ALIRO_UD_SYSTEM_OFF_BUTTON1_SETTLE_MS`), not immediately: arming
 * too early risks the Button 0 wake press cross-triggering this handler on
 * this chip family (docs/system_off_proposal.md caveat #2; see also the
 * matching guard in `HandleButton1Press()`). A no-op stub (with a one-time
 * warning) if no `sw1` devicetree alias exists.
 *
 * Call exactly once, from `AliroUd::Power::Start()`.
 */
void Init();

} // namespace AliroUd::Power::Button1
