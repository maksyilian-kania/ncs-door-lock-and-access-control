/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#pragma once

/*
 * Visible "device awake / CLI reachable" indication on `led1`
 * (docs/system_off_proposal.md). Deliberately separate from
 * AliroUd::Authorization::Indicator (`led0`), which keeps its existing,
 * unrelated "authorization required" role unchanged.
 */
namespace AliroUd::Power::Indicator {

/**
 * @brief Sets the awake indication on/off: on at boot (any wake source),
 * off immediately before `sys_poweroff()`.
 */
void SetAwake(bool awake);

} // namespace AliroUd::Power::Indicator
