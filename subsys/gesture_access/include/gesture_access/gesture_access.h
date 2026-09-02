/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#pragma once

namespace DoorLock::GestureAccess {

/**
 * @brief Initialize the gesture access feature.
 *
 * @return 0 on success, negative errno otherwise.
 */
int Init();

/**
 * @brief Start periodic capture + gesture inference.
 *
 * @return 0 on success, negative errno otherwise.
 */
int Start();

/**
 * @brief Stop periodic capture + gesture inference.
 */
void Stop();

/**
 * @brief Fired on confirmed (debounced) detection state transitions only - not
 * on every per-frame model result.
 *
 * @param detected True once a gesture has been seen for enough consecutive
 * frames to be confirmed, false again once it has been absent for enough
 * consecutive frames.
 */
using DetectionCallback = void (*)(bool detected);

/**
 * @brief Register a callback invoked from the gesture access capture thread's
 * context whenever the debounced detection state changes.
 *
 * Callbacks must be fast and non-blocking, and must marshal onto their own
 * required thread or event loop (e.g. the Matter/CHIP event loop) before
 * touching app-specific lock state.
 *
 * @param callback The callback to register.
 *
 * @return 0 on success, -ENOMEM if the callback registry is full.
 */
int RegisterDetectionCallback(DetectionCallback callback);

} // namespace DoorLock::GestureAccess
