/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#pragma once

namespace DoorLock::GestureAccess {


// Initialize the gesture access feature.
int Init();

// Start periodic capture + gesture inference.
int Start();

// Stop periodic capture + gesture inference.
void Stop();

} // namespace DoorLock::GestureAccess
