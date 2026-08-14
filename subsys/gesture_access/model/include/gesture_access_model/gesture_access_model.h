/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#ifndef DOOR_LOCK_GESTURE_ACCESS_MODEL_H_
#define DOOR_LOCK_GESTURE_ACCESS_MODEL_H_

#include <cstddef>
#include <cstdint>

namespace DoorLock::GestureAccess::Model {

constexpr size_t kInputWidth = 96;
constexpr size_t kInputHeight = 96;
constexpr size_t kInputSize = kInputWidth * kInputHeight;

#ifdef CONFIG_DOOR_LOCK_GESTURE_ACCESS_FRAME_FORWARDING
/* Debug/visualization cap on how many above-threshold grid cells get
 * reported per frame; not a limit on how many objects the model can
 * actually see. Only used by frame forwarding - the on-device detection
 * decision below is unaffected by this cap. */
constexpr size_t kMaxReportedDetections = 16;
#endif // CONFIG_DOOR_LOCK_GESTURE_ACCESS_FRAME_FORWARDING

struct Result {
	/* Debounced consumers (UpdateDebounce()/DetectionCallback) only ever
	 * need this and nothing else below. */
	bool detected;
	/* Strongest grid cell's confidence, every frame regardless of
	 * `detected` - useful for live threshold tuning. */
	uint16_t confidenceMilli;
	uint32_t inferenceTimeUs;

#ifdef CONFIG_DOOR_LOCK_GESTURE_ACCESS_FRAME_FORWARDING
	/* Every grid cell whose activation clears the detection threshold,
	 * reported as independent points so FOMO's
	 * multi-object detections are all visible on the host. Debug-only:
	 * on-device logic never reads this. */
	struct Detection {
		uint16_t x;
		uint16_t y;
		uint16_t confidenceMilli;
	} detections[kMaxReportedDetections];
	size_t detectionCount;
#endif // CONFIG_DOOR_LOCK_GESTURE_ACCESS_FRAME_FORWARDING
};

int Init();
int Run(const uint8_t *frame, size_t frameSize, Result &result);

} // namespace DoorLock::GestureAccess::Model

#endif // DOOR_LOCK_GESTURE_ACCESS_MODEL_H_
