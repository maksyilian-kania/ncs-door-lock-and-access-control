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
 * Streams captured grayscale frames to a host PC over the SoC's native USB
 * port (CDC ACM), following the wire format and structure of
 * frame_stream.{c,h} in the sdk-edge-ai photo_gesture_detection sample, minus
 * the RGB565 decode step since frames here are already single-channel
 * grayscale bytes:
 *   - a 16-byte binary header: magic "GAFF", version, pixel format, width,
 *     height, metadata length, pixel data length (all little-endian)
 *   - metadata bytes (ASCII JSON, may be empty)
 *   - raw grayscale pixel data (one byte per pixel)
 *   - a 2-byte little-endian CRC-16/CCITT trailer over the metadata+pixel
 *     bytes (matching Zephyr's crc16_ccitt(), seed 0xffff)
 * so that scripts/gesture_frame_viewer.py can decode it.
 */
namespace DoorLock::GestureAccess::FrameForwarding {

/**
 * @brief Bring up the USB device and the serial frame forwarding channel.
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
 * Frames are only sent while a host has the channel open (HostReady());
 * callers should typically skip building @p meta and calling this function
 * at all when that is not the case.
 *
 * @param width Frame width in pixels.
 * @param height Frame height in pixels.
 * @param pixels Raw grayscale pixel buffer, width * height bytes.
 * @param meta Metadata describing the inference result (JSON), may be null.
 * @param metaLength Length of @p meta in bytes.
 * @return 0 on success, -ENOTCONN if no host is attached, -ETIMEDOUT if the
 *         transfer did not complete in time, or another negative error code.
 */
int Send(uint16_t width, uint16_t height, const uint8_t *pixels, const char *meta, size_t metaLength);

} // namespace DoorLock::GestureAccess::FrameForwarding
