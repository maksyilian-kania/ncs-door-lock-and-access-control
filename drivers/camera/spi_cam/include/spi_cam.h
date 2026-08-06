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
 * TODO: add any camera-specific controls that do not fit the generic video
 * API (e.g. sensor-specific effects/gain) once the hardware is bring-up.
 */

#ifdef __cplusplus
extern "C" {
#endif

#ifdef __cplusplus
}
#endif

#endif /* DOOR_LOCK_DRIVERS_SPI_CAM_H_ */
