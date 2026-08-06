/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include <gesture_access/gesture_access.h>

#ifdef CONFIG_DOOR_LOCK_GESTURE_ACCESS_FRAME_FORWARDING
#include "frame_forwarding.h"
#endif // CONFIG_DOOR_LOCK_GESTURE_ACCESS_FRAME_FORWARDING

#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(door_lock_gesture_access, CONFIG_DOOR_LOCK_GESTURE_ACCESS_LOG_LEVEL);

namespace DoorLock::GestureAccess {

namespace {

/*
 * TODO: bring up the camera device (DEVICE_DT_GET of the door-lock,spi-cam
 * instance) and the inference runtime from sdk-edge-ai here, mirroring
 * applications/photo_gesture_detection/src/main.c in sdk-edge-ai:
 *   - video_set_format() / video_enqueue() the capture buffers
 *   - a capture timer/work item driving video_stream_start()+dequeue()
 *   - run_classifier() (or the Axon equivalent) on the captured frame
 */

} // namespace

int Init()
{
	LOG_INF("Gesture access init (skeleton, not yet implemented)");

#ifdef CONFIG_DOOR_LOCK_GESTURE_ACCESS_FRAME_FORWARDING
	int err = FrameForwarding::Init();
	if (err) {
		LOG_WRN("Frame forwarding unavailable (err %d)", err);
	}
#endif // CONFIG_DOOR_LOCK_GESTURE_ACCESS_FRAME_FORWARDING

	return 0;
}

int Start()
{
	/* TODO: start the capture/inference loop. */
	return 0;
}

void Stop()
{
	/* TODO: stop the capture/inference loop. */
}

} // namespace DoorLock::GestureAccess
