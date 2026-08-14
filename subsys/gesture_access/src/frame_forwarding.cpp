/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include "frame_forwarding.h"

#include <zephyr/logging/log.h>

LOG_MODULE_DECLARE(door_lock_gesture_access);

namespace DoorLock::GestureAccess::FrameForwarding {

int Init()
{
	// Bring up the serial channel.
	return 0;
}

bool HostReady()
{
	// Check whether a host is currently attached to the channel.
	return false;
}

int Send(uint16_t width, uint16_t height, const uint8_t *pixels, const char *meta, size_t metaLength)
{
	ARG_UNUSED(width);
	ARG_UNUSED(height);
	ARG_UNUSED(pixels);
	ARG_UNUSED(meta);
	ARG_UNUSED(metaLength);

	// Frame the header + pixel data + CRC and write it out.
	return -ENOSYS;
}

} // namespace DoorLock::GestureAccess::FrameForwarding
