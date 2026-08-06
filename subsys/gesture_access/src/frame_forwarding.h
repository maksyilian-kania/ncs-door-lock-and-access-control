/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#pragma once

#include <cstddef>
#include <cstdint>

/*
 * Internal helper for CONFIG_DOOR_LOCK_GESTURE_ACCESS_FRAME_FORWARDING.
 * Not part of the public gesture_access API (see gesture_access.h).
 *
 * TODO: implement against a serial channel (USB CDC ACM or UART), following
 * the wire format and structure of frame_stream.{c,h} in the sdk-edge-ai
 * photo_gesture_detection sample:
 *   - a small binary header (magic/version/pixel format/width/height/
 *     metadata length/data length)
 *   - raw pixel data
 *   - a CRC trailer
 * so that scripts/gesture_frame_viewer.py can decode it without changes.
 */
namespace DoorLock::GestureAccess::FrameForwarding {

/**
 * @brief Bring up the serial frame forwarding channel.
 *
 * @return 0 on success, or a negative error code on failure.
 */
int Init();

/**
 * @brief Check whether a host is currently attached to the channel.
 *
 * @return true if a host is ready to receive frames.
 */
bool HostReady();

/**
 * @brief Send one frame plus its inference metadata to the host.
 *
 * @param width Frame width in pixels.
 * @param height Frame height in pixels.
 * @param pixels Raw pixel buffer.
 * @param meta Metadata describing the inference result (e.g. JSON), may be null.
 * @param metaLength Length of @p meta in bytes.
 * @return 0 on success, or a negative error code on failure.
 */
int Send(uint16_t width, uint16_t height, const uint8_t *pixels, const char *meta, size_t metaLength);

} // namespace DoorLock::GestureAccess::FrameForwarding
