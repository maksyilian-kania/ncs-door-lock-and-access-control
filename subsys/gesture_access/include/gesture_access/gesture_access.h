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
 * Brings up the SPI camera device and the inference runtime, but does not
 * start capturing yet.
 *
 * @return 0 on success, or a negative error code on failure.
 */
int Init();

/**
 * @brief Start periodic capture + gesture inference.
 *
 * @return 0 on success, or a negative error code on failure.
 */
int Start();

/**
 * @brief Stop periodic capture + gesture inference.
 */
void Stop();

} // namespace DoorLock::GestureAccess
