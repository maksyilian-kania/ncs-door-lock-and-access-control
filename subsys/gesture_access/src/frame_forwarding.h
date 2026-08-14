/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#pragma once

#include <cstddef>
#include <cstdint>

// Internal helper for CONFIG_DOOR_LOCK_GESTURE_ACCESS_FRAME_FORWARDING.
namespace DoorLock::GestureAccess::FrameForwarding {

// Bring up the serial frame forwarding channel.
int Init();

// Check whether a host is currently attached to the channel.
bool HostReady();

// Send frame and its inference metadata to the host.
int Send(uint16_t width, uint16_t height, const uint8_t *pixels, const char *meta, size_t metaLength);

} // namespace DoorLock::GestureAccess::FrameForwarding
