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
constexpr size_t kMaxDetections = 12 * 12;

struct BoundingBox {
	const char *label;
	uint16_t x;
	uint16_t y;
	uint16_t width;
	uint16_t height;
	uint16_t confidenceMilli;
};

struct Result {
	BoundingBox boxes[kMaxDetections];
	size_t boxCount;
	uint32_t inferenceTimeUs;
};

int Init();
int Run(const uint8_t *frame, size_t frameSize, Result &result);

} // namespace DoorLock::GestureAccess::Model

#endif // DOOR_LOCK_GESTURE_ACCESS_MODEL_H_
