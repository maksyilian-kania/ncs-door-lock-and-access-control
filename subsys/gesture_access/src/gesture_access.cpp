/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include <gesture_access/gesture_access.h>

#ifdef CONFIG_DOOR_LOCK_GESTURE_ACCESS_FRAME_FORWARDING
#include "frame_forwarding.h"
#endif // CONFIG_DOOR_LOCK_GESTURE_ACCESS_FRAME_FORWARDING

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/video.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/pm/device.h>
#include <zephyr/sys/util.h>

#include <gesture_access_workqueue/gesture_access_workqueue.h>

#include <cerrno>
#include <cstdio>
#include <cstring>

#include <edge-impulse-sdk/classifier/ei_classifier_types.h>
#include <edge-impulse-sdk/dsp/numpy_types.h>

LOG_MODULE_REGISTER(door_lock_gesture_access, CONFIG_DOOR_LOCK_GESTURE_ACCESS_LOG_LEVEL);

/*
 * Declared here rather than including ei_run_classifier.h: that header
 * defines run_classifier() and friends as plain (non-inline) extern "C"
 * functions, meant to be compiled exactly once - by
 * edge-impulse-sdk/classifier/ei_run_classifier_c.cpp, which
 * subsys/gesture_access/model/CMakeLists.txt already builds into the app.
 * Including it a second time here would give the linker two definitions of
 * every classifier symbol ("multiple definition of `run_classifier'" etc).
 * Same pattern edge-ai's photo_gesture_detection sample uses in main.c - but
 * that file is plain C, where dsp/numpy_types.h's "namespace ei" (guarded by
 * #ifdef __cplusplus) doesn't apply and signal_t is simply global. Here it's
 * ei::signal_t, hence the using-declaration below.
 */
using ei::signal_t;
extern "C" EI_IMPULSE_ERROR run_classifier(signal_t *signal, ei_impulse_result_t *result, bool debug);

namespace DoorLock::GestureAccess {

namespace {


constexpr uint32_t kCaptureWidth = 96;
constexpr uint32_t kCaptureHeight = 96;
constexpr uint32_t kFrameBytes = kCaptureWidth * kCaptureHeight;


constexpr k_timeout_t kDequeueRetryDelay = K_MSEC(5);

const struct device *sCamDev;

/*
 * Held in its captured grayscale form and decoded per pixel in
 * GetImageData() rather than converted to a float matrix up front - mirrors
 * sdk-edge-ai's applications/photo_gesture_detection, whose
 * rgb565_to_ei_pixel() comment explains why: run_classifier() already needs
 * its own float matrix of the whole frame on the heap, so a second copy
 * here doesn't need to exist.
 */
uint8_t sFrameBuf[kFrameBytes];

bool sStreaming;
k_work_delayable sInferenceWork;
uint32_t sCapturedFrameSeq;
uint32_t sPreviousFrameChecksum;
uint32_t sRepeatedFrameCount;

/**
 * @brief Decode one grayscale byte into the packed-RGB888-in-a-float
 * representation the Edge Impulse image DSP block expects, replicating the
 * gray value across R/G/B (same convention as
 * sdk-edge-ai/applications/photo_gesture_detection's rgb565_to_ei_pixel(),
 * just starting from a single gray channel instead of RGB565).
 */
inline float GrayToEiPixel(uint8_t gray)
{
	const uint32_t rgb = ((uint32_t)gray << 16) | ((uint32_t)gray << 8) | gray;

	return (float)rgb;
}

int GetImageData(size_t offset, size_t length, float *out_ptr)
{
	for (size_t i = 0; i < length; i++) {
		out_ptr[i] = GrayToEiPixel(sFrameBuf[offset + i]);
	}

	return 0;
}

void LogFrameDiagnostics()
{
	uint8_t minValue = UINT8_MAX;
	uint8_t maxValue = 0;
	uint32_t sum = 0;
	uint32_t horizontalDeltaSum = 0;
	uint32_t checksum = 2166136261U;

	for (size_t i = 0; i < ARRAY_SIZE(sFrameBuf); i++) {
		const uint8_t value = sFrameBuf[i];

		minValue = MIN(minValue, value);
		maxValue = MAX(maxValue, value);
		sum += value;
		checksum = (checksum ^ value) * 16777619U;

		if ((i % kCaptureWidth) != 0U) {
			const uint8_t previous = sFrameBuf[i - 1U];
			horizontalDeltaSum += value > previous ? value - previous : previous - value;
		}
	}

	sCapturedFrameSeq++;
	if (checksum == sPreviousFrameChecksum) {
		sRepeatedFrameCount++;
	} else {
		sRepeatedFrameCount = 0;
	}
	sPreviousFrameChecksum = checksum;

	const uint32_t horizontalPairCount = kCaptureHeight * (kCaptureWidth - 1U);
	if (sCapturedFrameSeq <= 8U || (sCapturedFrameSeq % 32U) == 0U) {
		LOG_INF("Frame %u: min=%u max=%u avg=%u avg_dx=%u checksum=%08x repeats=%u",
			sCapturedFrameSeq, minValue, maxValue, sum / kFrameBytes,
			horizontalDeltaSum / horizontalPairCount, checksum, sRepeatedFrameCount);
	}

	if ((uint32_t)(maxValue - minValue) < 8U) {
		LOG_WRN("Frame %u has almost no luminance range", sCapturedFrameSeq);
	}
	if (sRepeatedFrameCount == 4U) {
		LOG_WRN("Camera returned five identical frames");
	}
}

void LogDetectionResult(const ei_impulse_result_t &result)
{
	if (result.bounding_boxes_count > 0) {
		LOG_INF("gesture detected: %s (score %.3f, boxes %u) at (%u,%u)",
			result.bounding_boxes[0].label, (double)result.bounding_boxes[0].value,
			result.bounding_boxes_count, result.bounding_boxes[0].x,
			result.bounding_boxes[0].y);

		/*
		 * TODO (next step): call into the app-specific unlock entry
		 * point here. Each application wires this up differently
		 * today (Aliro::LockSimInstance().Unlock() vs.
		 * BoltLockManager::Unlock(...)); this feature is shared
		 * subsys code used by all three apps, so it needs a
		 * callback-registration API analogous to
		 * subsys/nus_service's RegisterCommand(), not a direct call
		 * to any one app's unlock function. Deliberately left as a
		 * log-only stub for now - see Camera driver plan v2.md
		 * section 8.
		 */
	} else {
		LOG_INF("no gesture");
	}
}

#ifdef CONFIG_DOOR_LOCK_GESTURE_ACCESS_FRAME_FORWARDING

/*
 * Cap on how many of result.bounding_boxes[] get serialized into the
 * metadata JSON below. EI_CLASSIFIER_OBJECT_DETECTION_COUNT (10) is only the
 * *guaranteed minimum* the model reports (see the warning in
 * model_metadata.h) - bounding_boxes_count can exceed it - so this is a
 * separate, deliberately small bound to keep sMetaBuf's size fixed.
 */
constexpr uint32_t kMaxForwardedBoxes = 16;
constexpr size_t kMetaBufSize = 128 + (kMaxForwardedBoxes * 80);

uint32_t sFrameSeq;
char sMetaBuf[kMetaBufSize];

/*
 * Builds the JSON metadata payload consumed by
 * scripts/gesture_frame_viewer.py: {"seq":..,"infer_ms":..,"boxes":[
 * {"label":"..","value":..,"x":..,"y":..,"w":..,"h":..}, ...]}.
 * Returns the number of bytes written, or 0 if it would not fit (in which
 * case the frame is still forwarded, just without metadata).
 */
size_t BuildDetectionMeta(const ei_impulse_result_t &result)
{
	int written = snprintf(sMetaBuf, sizeof(sMetaBuf), "{\"seq\":%u,\"infer_ms\":%d,\"boxes\":[",
				sFrameSeq, result.timing.classification);

	if (written < 0 || (size_t)written >= sizeof(sMetaBuf)) {
		return 0;
	}

	size_t offset = (size_t)written;
	const uint32_t boxCount = MIN(result.bounding_boxes_count, kMaxForwardedBoxes);

	for (uint32_t i = 0; i < boxCount; i++) {
		const ei_impulse_result_bounding_box_t &box = result.bounding_boxes[i];

		if (box.value == 0) {
			continue;
		}

		/*
		 * Formatted as integer-thousandths (%u.%03u) rather than via
		 * "%f" so this does not depend on the C library's float
		 * formatting support being enabled - value is always in
		 * [0.0, 1.0], so this can't lose precision that matters here.
		 */
		const uint32_t valueMilli = (uint32_t)(box.value * 1000.0f + 0.5f);

		written = snprintf(&sMetaBuf[offset], sizeof(sMetaBuf) - offset,
				    "%s{\"label\":\"%s\",\"value\":%u.%03u,\"x\":%u,\"y\":%u,"
				    "\"w\":%u,\"h\":%u}",
				    (offset > 0 && sMetaBuf[offset - 1] != '[') ? "," : "",
				    box.label, valueMilli / 1000, valueMilli % 1000, box.x,
				    box.y, box.width, box.height);

		if (written < 0 || (size_t)written >= sizeof(sMetaBuf) - offset) {
			/* Would overflow: stop, close out what we have so far. */
			break;
		}

		offset += (size_t)written;
	}

	written = snprintf(&sMetaBuf[offset], sizeof(sMetaBuf) - offset, "]}");
	if (written < 0 || (size_t)written >= sizeof(sMetaBuf) - offset) {
		return 0;
	}

	return offset + (size_t)written;
}

void ForwardFrame(const ei_impulse_result_t &result)
{
	sFrameSeq++;

	if (!FrameForwarding::HostReady()) {
		return;
	}

	const size_t metaLen = BuildDetectionMeta(result);
	int err = FrameForwarding::Send(kCaptureWidth, kCaptureHeight, sFrameBuf,
					 metaLen > 0 ? sMetaBuf : nullptr, metaLen);

	if (err && err != -ENOTCONN) {
		LOG_WRN("Frame forwarding send failed (err %d)", err);
	}
}

#endif // CONFIG_DOOR_LOCK_GESTURE_ACCESS_FRAME_FORWARDING

void RunInference()
{
	ei_impulse_result_t result = {};
	signal_t features_signal = {
		.get_data = GetImageData,
		.total_length = EI_CLASSIFIER_RAW_SAMPLE_COUNT,
	};

	EI_IMPULSE_ERROR err = run_classifier(&features_signal, &result, false);
	if (err != EI_IMPULSE_OK) {
		LOG_WRN("Classification failed (err %d)", err);
		return;
	}

	LogDetectionResult(result);

#ifdef CONFIG_DOOR_LOCK_GESTURE_ACCESS_FRAME_FORWARDING
	ForwardFrame(result);
#endif // CONFIG_DOOR_LOCK_GESTURE_ACCESS_FRAME_FORWARDING
}

/*
 * Runs on gesture_access_workqueue - the same dedicated, single-threaded
 * queue the driver uses for its own capture/FIFO-drain work
 * (spi_cam_buffer_work()). Because it's single-threaded, this handler must
 * never block waiting for a frame: doing so would deadlock against the
 * driver's own work item, which is what would eventually satisfy that wait
 * but can never run while this handler is blocked on the same queue. Hence
 * video_dequeue() is called with K_NO_WAIT and -EAGAIN just reschedules,
 * rather than K_FOREVER. See Camera driver plan v2.md section 8 (2026-08-12
 * update) for the full rationale.
 */
void InferenceWorkHandler(struct k_work *work)
{
	ARG_UNUSED(work);

	if (!sStreaming) {
		return;
	}

	struct video_buffer *vbuf;
	int err = video_dequeue(sCamDev, &vbuf, K_NO_WAIT);

	if (err == -EAGAIN) {
		GestureAccessWorkqueueReschedule(&sInferenceWork, kDequeueRetryDelay);
		return;
	}
	if (err) {
		LOG_WRN("video_dequeue failed (err %d), retrying", err);
		GestureAccessWorkqueueReschedule(&sInferenceWork, kDequeueRetryDelay);
		return;
	}

	if (vbuf->bytesused != sizeof(sFrameBuf)) {
		LOG_WRN("Dropping malformed frame: got %zu bytes, expected %zu", vbuf->bytesused,
			sizeof(sFrameBuf));
		vbuf->type = VIDEO_BUF_TYPE_OUTPUT;
		video_enqueue(sCamDev, vbuf);
		GestureAccessWorkqueueReschedule(&sInferenceWork, kDequeueRetryDelay);
		return;
	}

	memcpy(sFrameBuf, vbuf->buffer, sizeof(sFrameBuf));

	vbuf->type = VIDEO_BUF_TYPE_OUTPUT;
	video_enqueue(sCamDev, vbuf);

	LogFrameDiagnostics();

	/*
	 * run_classifier() is event-blocking on the Axon NPU (ISR + semaphore,
	 * not busy-polling - confirmed by tracing
	 * edge-ai/drivers/axon/nrf_axon_nn_infer.c, see plan section 5), so
	 * running it inline here is safe: the CPU sleeps for the inference
	 * duration instead of stalling other threads.
	 */
	RunInference();

	GestureAccessWorkqueueReschedule(&sInferenceWork, kDequeueRetryDelay);
}

} // namespace

int Init()
{
	int err;

	sCamDev = DEVICE_DT_GET(DT_NODELABEL(door_lock_spi_cam));

	if (!device_is_ready(sCamDev)) {
		LOG_ERR("SPI camera device not ready");
		return -ENODEV;
	}

	struct video_format fmt = {
		.type = VIDEO_BUF_TYPE_OUTPUT,
		.pixelformat = VIDEO_PIX_FMT_GREY,
		.width = kCaptureWidth,
		.height = kCaptureHeight,
	};

	err = video_set_format(sCamDev, &fmt);
	if (err) {
		LOG_ERR("Failed to set camera format (err %d)", err);
		return err;
	}

	static struct video_buffer *vbufs[2];

	for (size_t i = 0; i < ARRAY_SIZE(vbufs); i++) {
		vbufs[i] = video_buffer_alloc(kFrameBytes, K_NO_WAIT);
		if (vbufs[i] == nullptr) {
			LOG_ERR("Failed to allocate video buffer %zu", i);
			return -ENOMEM;
		}
		vbufs[i]->type = VIDEO_BUF_TYPE_OUTPUT;
		video_enqueue(sCamDev, vbufs[i]);
	}

	k_work_init_delayable(&sInferenceWork, InferenceWorkHandler);

	/*
	 * gesture_access_workqueue self-starts via SYS_INIT at APPLICATION
	 * level (mirroring aliro_workqueue) - no explicit start call needed
	 * here.
	 */

	LOG_INF("Gesture access init done (model: %s, %ux%u grayscale)",
		EI_CLASSIFIER_PROJECT_NAME, kCaptureWidth, kCaptureHeight);

#ifdef CONFIG_DOOR_LOCK_GESTURE_ACCESS_FRAME_FORWARDING
	err = FrameForwarding::Init();
	if (err) {
		LOG_WRN("Frame forwarding unavailable (err %d)", err);
	}
#endif // CONFIG_DOOR_LOCK_GESTURE_ACCESS_FRAME_FORWARDING

	return 0;
}

int Start()
{
	int err;

	/*
	 * Resume the sensor before (re-)starting capture - this is what
	 * actually wakes it (see the VIDEO_CID_SPI_CAM_LOWPOWER doc comment
	 * in spi_cam.h), not the capture cadence alone.
	 */
	err = pm_device_action_run(sCamDev, PM_DEVICE_ACTION_RESUME);
	if (err && err != -EALREADY) {
		LOG_ERR("Failed to resume camera (err %d)", err);
		return err;
	}

	err = video_stream_start(sCamDev, VIDEO_BUF_TYPE_OUTPUT);
	if (err) {
		LOG_ERR("Failed to start camera stream (err %d)", err);
		return err;
	}

	sStreaming = true;

	/*
	 * No second cadence timer here: the driver's own
	 * VIDEO_CID_SPI_CAM_CAPTURE_INTERVAL_MS timer (section 3) remains the
	 * only capture cadence. This work item just opportunistically drains
	 * whatever it produces.
	 */
	GestureAccessWorkqueueReschedule(&sInferenceWork, K_NO_WAIT);

	return 0;
}

void Stop()
{
	sStreaming = false;

	int err = video_stream_stop(sCamDev, VIDEO_BUF_TYPE_OUTPUT);
	if (err) {
		LOG_WRN("Failed to stop camera stream (err %d)", err);
	}

	(void)k_work_cancel_delayable(&sInferenceWork);

	/*
	 * Battery/low-power door lock: sleep the sensor, not just stop the
	 * capture cadence - see the VIDEO_CID_SPI_CAM_LOWPOWER doc comment in
	 * spi_cam.h and Camera driver plan v2.md section 4.
	 */
	err = pm_device_action_run(sCamDev, PM_DEVICE_ACTION_SUSPEND);
	if (err && err != -EALREADY) {
		LOG_WRN("Failed to suspend camera (err %d)", err);
	}
}

} // namespace DoorLock::GestureAccess
