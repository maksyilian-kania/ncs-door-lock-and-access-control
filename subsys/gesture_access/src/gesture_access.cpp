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
	/*
	 * TODO: start the capture/inference loop. Before (re-)starting
	 * capture, resume the camera device via
	 * pm_device_action_run(cam_dev, PM_DEVICE_ACTION_RESUME) - see the
	 * VIDEO_CID_SPI_CAM_LOWPOWER doc comment in spi_cam.h for why this,
	 * rather than the capture cadence alone, is what actually wakes the
	 * sensor.
	 */
	return 0;
}

void Stop()
{
	/*
	 * TODO: stop the capture/inference loop, then suspend the camera
	 * device via pm_device_action_run(cam_dev, PM_DEVICE_ACTION_SUSPEND)
	 * - decided in Camera driver plan v2.md section 4: this is a
	 * battery/low-power door lock, so sleeping the sensor is correct,
	 * not just stopping the capture timer. See the VIDEO_CID_SPI_CAM_LOWPOWER
	 * doc comment in spi_cam.h.
	 */
}

} // namespace DoorLock::GestureAccess
