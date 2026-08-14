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

// Fired on confirmed (debounced) detection state transitions only - not on
// every per-frame model result. `detected` is true once a gesture has been
// seen for enough consecutive frames to be confirmed, and false again once
// it has been absent for enough consecutive frames.
using DetectionCallback = void (*)(bool detected);

// Register a callback invoked from the gesture access capture thread's
// context whenever the debounced detection state changes. Callbacks must be
// fast and non-blocking, and must marshal onto their own required thread or
// event loop (e.g. the Matter/CHIP event loop) before touching app-specific
// lock state. Returns -ENOMEM if the callback registry is full.
int RegisterDetectionCallback(DetectionCallback callback);

} // namespace DoorLock::GestureAccess
