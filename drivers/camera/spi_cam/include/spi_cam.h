/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#ifndef DOOR_LOCK_DRIVERS_SPI_CAM_H_
#define DOOR_LOCK_DRIVERS_SPI_CAM_H_

/**
 * @file
 * @brief Public API for the SPI camera driver used by gesture access.
 *
 * The driver implements Zephyr's `video` driver class
 * (@ref zephyr/drivers/video.h), so consumers should use the generic
 * `video_*()` calls (video_set_format(), video_stream_start(),
 * video_enqueue()/video_dequeue(), ...) instead of anything declared here.
 *
 * This header only adds the vendor-specific controls that do not fit the
 * generic video control IDs, following the same pattern as
 * zephyr/include/zephyr/drivers/video/arducam_mega.h.
 *
 * The only supported video format is 96x96 VIDEO_PIX_FMT_GREY. The sensor
 * captures YUYV into an internal driver buffer that is overwritten for every
 * frame; dequeued caller-owned buffers contain exactly 9216 grayscale bytes.
 */

#include <zephyr/drivers/video-controls.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Trigger a reset of the camera. */
#define VIDEO_CID_SPI_CAM_RESET (VIDEO_CID_PRIVATE_BASE + 1)

/**
 * @brief Trigger the entry (1) or exit (0) of low-power mode.
 *
 * Also reachable through Zephyr's device PM for this driver's device
 * (PM_DEVICE_ACTION_SUSPEND/PM_DEVICE_ACTION_RESUME) instead of setting this
 * control directly - see spi_cam_pm_action() in spi_cam.c. That is the
 * intended integration point for gesture access: `Stop()` should suspend
 * this device (`pm_device_action_run(cam_dev, PM_DEVICE_ACTION_SUSPEND)`),
 * not just stop the capture cadence, since this is a battery/low-power door
 * lock; `Start()` should resume it the same way before re-enabling capture.
 */
#define VIDEO_CID_SPI_CAM_LOWPOWER (VIDEO_CID_PRIVATE_BASE + 2)

/**
 * @brief Capture cadence, in milliseconds, driving the gesture-access
 * sampling rate.
 *
 * This governs only how often this driver captures+delivers a frame; it is
 * independent of the door lock's own event loop and of any other consumer's
 * timing. The driver enforces a minimum value (see
 * CONFIG_DOOR_LOCK_CAMERA_SPI_CAPTURE_INTERVAL_MS_DEFAULT) based on the
 * worst-case capture + FIFO-read latency of the currently configured
 * resolution/format; requests below that floor are clamped up, not
 * silently ignored.
 */
#define VIDEO_CID_SPI_CAM_CAPTURE_INTERVAL_MS (VIDEO_CID_PRIVATE_BASE + 3)

#ifdef __cplusplus
}
#endif

#endif /* DOOR_LOCK_DRIVERS_SPI_CAM_H_ */
