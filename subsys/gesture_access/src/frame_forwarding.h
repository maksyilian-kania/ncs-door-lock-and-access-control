/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#pragma once

#include <cstddef>
#include <cstdint>

/**
 * @brief Internal helper for CONFIG_DOOR_LOCK_GESTURE_ACCESS_FRAME_FORWARDING.
 */
namespace DoorLock::GestureAccess::FrameForwarding {

/**
 * @brief Bring up the serial frame forwarding channel.
 *
 * @return 0 on success, negative errno otherwise.
 */
int Init();

/**
 * @brief Check whether a host is currently attached to the channel.
 *
 * @return True if a host has the port open, false otherwise.
 */
bool HostReady();

/**
 * @brief Send a frame and its inference metadata to the host.
 *
 * @param width Frame width in pixels.
 * @param height Frame height in pixels.
 * @param pixels Grayscale pixel buffer, width * height bytes.
 * @param meta Inference metadata to send alongside the frame, as a JSON string.
 * @param metaLength Length of `meta` in bytes.
 *
 * @return 0 on success, negative errno otherwise.
 */
int Send(uint16_t width, uint16_t height, const uint8_t *pixels, const char *meta, size_t metaLength);

} // namespace DoorLock::GestureAccess::FrameForwarding
