/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include <gesture_access/gesture_access.h>

#ifdef CONFIG_DOOR_LOCK_GESTURE_ACCESS_FRAME_FORWARDING
#include "frame_forwarding.h"
#endif // CONFIG_DOOR_LOCK_GESTURE_ACCESS_FRAME_FORWARDING

#include <gesture_access_model/gesture_access_model.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/video-controls.h>
#include <zephyr/drivers/video.h>
#include <zephyr/drivers/video/arducam_mega.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/util.h>

#include <cerrno>
#include <cstdint>
#include <cstdio>

LOG_MODULE_REGISTER(door_lock_gesture_access, CONFIG_DOOR_LOCK_GESTURE_ACCESS_LOG_LEVEL);

namespace DoorLock::GestureAccess {

namespace {

constexpr uint16_t kFrameWidth = Model::kInputWidth;
constexpr uint16_t kFrameHeight = Model::kInputHeight;
constexpr size_t kVideoBufferCount = CONFIG_VIDEO_BUFFER_POOL_NUM_MAX;
constexpr size_t kMaxDetectionCallbacks = 4;

const device *sVideoDevice = DEVICE_DT_GET(DT_NODELABEL(arducam_mega));

k_sem sActivateSignal;
atomic_t sStopRequested;
uint8_t sGrayscaleFrame[Model::kInputSize];
uint8_t sConfirmCount;
uint8_t sReleaseCount;
bool sConfirmedDetected;

DetectionCallback sDetectionCallbacks[kMaxDetectionCallbacks];
size_t sDetectionCallbackCount;

int SetCameraLowPower(bool enable)
{
	video_control ctrl{
		.id = VIDEO_CID_ARDUCAM_LOWPOWER,
		.val = enable ? 1 : 0,
	};

	return video_set_ctrl(sVideoDevice, &ctrl);
}

void NotifyDetectionCallbacks(bool detected)
{
	for (size_t i = 0; i < sDetectionCallbackCount; i++) {
		sDetectionCallbacks[i](detected);
	}
}

void UpdateDebounce(bool frameHasDetection)
{
	if (frameHasDetection) {
		sReleaseCount = 0;
		sConfirmCount = MIN(sConfirmCount + 1, UINT8_MAX);
	} else {
		sConfirmCount = 0;
		sReleaseCount = MIN(sReleaseCount + 1, UINT8_MAX);
	}

	if (!sConfirmedDetected && sConfirmCount >= CONFIG_DOOR_LOCK_GESTURE_ACCESS_DEBOUNCE_CONFIRM_FRAMES) {
		sConfirmedDetected = true;
		LOG_INF("Gesture detected");
		NotifyDetectionCallbacks(true);
	} else if (sConfirmedDetected && sReleaseCount >= CONFIG_DOOR_LOCK_GESTURE_ACCESS_DEBOUNCE_RELEASE_FRAMES) {
		sConfirmedDetected = false;
		LOG_INF("Gesture no longer detected");
		NotifyDetectionCallbacks(false);
	}
}

void ExtractGrayscale(const video_buffer &vbuf, size_t &grayscaleBytesFilled)
{
	const size_t samples = MIN(vbuf.bytesused / 2, Model::kInputSize - grayscaleBytesFilled);

	for (size_t i = 0; i < samples; i++) {
		sGrayscaleFrame[grayscaleBytesFilled + i] = vbuf.buffer[i * 2];
	}

	grayscaleBytesFilled += samples;
}

int CaptureAndInferOneFrame()
{
	size_t grayscaleBytesFilled = 0;

	while (grayscaleBytesFilled < Model::kInputSize) {
		video_buffer *vbuf;
		int err = video_dequeue(sVideoDevice, &vbuf, K_MSEC(1000));

		if (err) {
			LOG_ERR("video_dequeue failed (err %d)", err);
			return err;
		}

		ExtractGrayscale(*vbuf, grayscaleBytesFilled);

		err = video_enqueue(sVideoDevice, vbuf);
		if (err) {
			LOG_ERR("video_enqueue failed (err %d)", err);
			return err;
		}
	}

	Model::Result result;
	int err = Model::Run(sGrayscaleFrame, Model::kInputSize, result);

	if (err) {
		LOG_ERR("Model::Run failed (err %d)", err);
		return err;
	}

	UpdateDebounce(result.detected);

#ifdef CONFIG_DOOR_LOCK_GESTURE_ACCESS_FRAME_FORWARDING
	if (FrameForwarding::HostReady()) {
		char meta[256];
		int offset = snprintf(meta, sizeof(meta), "{\"det\":%d,\"conf\":%u,\"us\":%u,\"pts\":[",
				       result.detected, result.confidenceMilli, result.inferenceTimeUs);

		for (size_t i = 0; i < result.detectionCount && offset > 0 && (size_t)offset < sizeof(meta); i++) {
			offset += snprintf(&meta[offset], sizeof(meta) - (size_t)offset,
					    "%s{\"x\":%u,\"y\":%u,\"conf\":%u}", i ? "," : "",
					    result.detections[i].x, result.detections[i].y,
					    result.detections[i].confidenceMilli);
		}

		if (offset > 0 && (size_t)offset < sizeof(meta) - 2) {
			offset += snprintf(&meta[offset], sizeof(meta) - (size_t)offset, "]}");
		}

		if (offset > 0 && (size_t)offset < sizeof(meta)) {
			FrameForwarding::Send(kFrameWidth, kFrameHeight, sGrayscaleFrame, meta,
					      static_cast<size_t>(offset));
		}
	}
#endif // CONFIG_DOOR_LOCK_GESTURE_ACCESS_FRAME_FORWARDING

	return 0;
}

void CaptureThreadFn(void *, void *, void *)
{
	for (;;) {
		k_sem_take(&sActivateSignal, K_FOREVER);

		sConfirmCount = 0;
		sReleaseCount = 0;
		sConfirmedDetected = false;

		int err = SetCameraLowPower(false);
		if (err) {
			LOG_ERR("Failed to wake camera (err %d)", err);
		}

		err = video_stream_start(sVideoDevice, VIDEO_BUF_TYPE_OUTPUT);
		if (err) {
			LOG_ERR("Failed to start video stream (err %d)", err);
		}

		while (!atomic_get(&sStopRequested)) {
			CaptureAndInferOneFrame();
		}

		video_stream_stop(sVideoDevice, VIDEO_BUF_TYPE_OUTPUT);
		SetCameraLowPower(true);

		atomic_clear(&sStopRequested);
	}
}

K_THREAD_DEFINE(sCaptureThread, CONFIG_DOOR_LOCK_GESTURE_ACCESS_THREAD_STACK_SIZE, CaptureThreadFn, NULL, NULL, NULL,
		CONFIG_DOOR_LOCK_GESTURE_ACCESS_THREAD_PRIORITY, 0, K_TICKS_FOREVER);

} // namespace

int Init()
{
	LOG_INF("Gesture access init");

	if (!device_is_ready(sVideoDevice)) {
		LOG_ERR("Video device not ready");
		return -ENODEV;
	}

	k_sem_init(&sActivateSignal, 0, 1);

	video_format fmt{
		.type = VIDEO_BUF_TYPE_OUTPUT,
		.pixelformat = VIDEO_PIX_FMT_YUYV,
		.width = kFrameWidth,
		.height = kFrameHeight,
		.pitch = static_cast<uint32_t>(kFrameWidth * 2),
	};

	int err = video_set_format(sVideoDevice, &fmt);
	if (err) {
		LOG_ERR("Failed to set video format (err %d)", err);
		return err;
	}

	for (size_t i = 0; i < kVideoBufferCount; i++) {
		video_buffer *vbuf = video_buffer_alloc(kFrameWidth * kFrameHeight * 2, K_NO_WAIT);

		if (vbuf == nullptr) {
			LOG_ERR("Failed to allocate video buffer %zu", i);
			return -ENOMEM;
		}

		vbuf->type = VIDEO_BUF_TYPE_OUTPUT;
		video_enqueue(sVideoDevice, vbuf);
	}

	err = Model::Init();
	if (err) {
		LOG_ERR("Failed to init model (err %d)", err);
		return err;
	}

#ifdef CONFIG_DOOR_LOCK_GESTURE_ACCESS_FRAME_FORWARDING
	err = FrameForwarding::Init();
	if (err) {
		LOG_WRN("Frame forwarding unavailable (err %d)", err);
	}
#endif // CONFIG_DOOR_LOCK_GESTURE_ACCESS_FRAME_FORWARDING

	k_thread_start(sCaptureThread);

	return 0;
}

int Start()
{
	atomic_clear(&sStopRequested);
	k_sem_give(&sActivateSignal);

	return 0;
}

void Stop()
{
	atomic_set(&sStopRequested, 1);
}

int RegisterDetectionCallback(DetectionCallback callback)
{
	if (sDetectionCallbackCount >= kMaxDetectionCallbacks) {
		return -ENOMEM;
	}

	sDetectionCallbacks[sDetectionCallbackCount++] = callback;
	return 0;
}

} // namespace DoorLock::GestureAccess
