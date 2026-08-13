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

#include <gesture_access_model/gesture_access_model.h>
#include <gesture_access_workqueue/gesture_access_workqueue.h>

#include <cerrno>
#include <cstdio>
#include <cstring>

LOG_MODULE_REGISTER(door_lock_gesture_access, CONFIG_DOOR_LOCK_GESTURE_ACCESS_LOG_LEVEL);

namespace DoorLock::GestureAccess {

namespace {

constexpr uint32_t kCaptureWidth = 96;
constexpr uint32_t kCaptureHeight = 96;
constexpr uint32_t kFrameBytes = kCaptureWidth * kCaptureHeight;

constexpr k_timeout_t kDequeueRetryDelay = K_MSEC(5);

const struct device *sCamDev;

uint8_t sFrameBuf[kFrameBytes];

bool sStreaming;
k_work_delayable sInferenceWork;
uint32_t sCapturedFrameSeq;
uint32_t sPreviousFrameChecksum;
uint32_t sRepeatedFrameCount;

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
		LOG_INF("Frame %u: min=%u max=%u avg=%u avg_dx=%u checksum=%08x repeats=%u", sCapturedFrameSeq,
			minValue, maxValue, sum / kFrameBytes, horizontalDeltaSum / horizontalPairCount, checksum,
			sRepeatedFrameCount);
	}

	if ((uint32_t)(maxValue - minValue) < 8U) {
		LOG_WRN("Frame %u has almost no luminance range", sCapturedFrameSeq);
	}
	if (sRepeatedFrameCount == 4U) {
		LOG_WRN("Camera returned five identical frames");
	}
}

void LogDetectionResult(const Model::Result &result)
{
	if (result.boxCount > 0) {
		const Model::BoundingBox &box = result.boxes[0];

		LOG_INF("gesture detected: %s (score %u.%03u, boxes %u) at (%u,%u)", box.label,
			box.confidenceMilli / 1000U, box.confidenceMilli % 1000U,
			static_cast<unsigned int>(result.boxCount), box.x, box.y);

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

/* Keep USB metadata storage bounded even if many FOMO regions are detected. */
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
size_t BuildDetectionMeta(const Model::Result &result)
{
	int written = snprintf(sMetaBuf, sizeof(sMetaBuf), "{\"seq\":%u,\"infer_ms\":%u,\"boxes\":[", sFrameSeq,
			       result.inferenceTimeUs / 1000U);

	if (written < 0 || (size_t)written >= sizeof(sMetaBuf)) {
		return 0;
	}

	size_t offset = (size_t)written;
	const size_t boxCount = MIN(result.boxCount, static_cast<size_t>(kMaxForwardedBoxes));

	for (size_t i = 0; i < boxCount; i++) {
		const Model::BoundingBox &box = result.boxes[i];

		written = snprintf(&sMetaBuf[offset], sizeof(sMetaBuf) - offset,
				   "%s{\"label\":\"%s\",\"value\":%u.%03u,\"x\":%u,\"y\":%u,"
				   "\"w\":%u,\"h\":%u}",
				   (offset > 0 && sMetaBuf[offset - 1] != '[') ? "," : "", box.label,
				   box.confidenceMilli / 1000U, box.confidenceMilli % 1000U, box.x, box.y, box.width,
				   box.height);

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

void ForwardFrame(const Model::Result &result)
{
	sFrameSeq++;

	if (!FrameForwarding::HostReady()) {
		return;
	}

	const size_t metaLen = BuildDetectionMeta(result);
	int err = FrameForwarding::Send(kCaptureWidth, kCaptureHeight, sFrameBuf, metaLen > 0 ? sMetaBuf : nullptr,
					metaLen);

	if (err && err != -ENOTCONN) {
		LOG_WRN("Frame forwarding send failed (err %d)", err);
	}
}

#endif // CONFIG_DOOR_LOCK_GESTURE_ACCESS_FRAME_FORWARDING

void RunInference()
{
	Model::Result result = {};
	int err = Model::Run(sFrameBuf, sizeof(sFrameBuf), result);

	if (err) {
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
		LOG_WRN("Dropping malformed frame: got %zu bytes, expected %zu", vbuf->bytesused, sizeof(sFrameBuf));
		vbuf->type = VIDEO_BUF_TYPE_OUTPUT;
		video_enqueue(sCamDev, vbuf);
		GestureAccessWorkqueueReschedule(&sInferenceWork, kDequeueRetryDelay);
		return;
	}

	memcpy(sFrameBuf, vbuf->buffer, sizeof(sFrameBuf));

	vbuf->type = VIDEO_BUF_TYPE_OUTPUT;
	video_enqueue(sCamDev, vbuf);

	LogFrameDiagnostics();

	/* Axon synchronous inference sleeps on an event semaphore, not a busy loop. */
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

	err = Model::Init();
	if (err) {
		LOG_ERR("Failed to initialize Axon gesture model (err %d)", err);
		return err;
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

	LOG_INF("Gesture access init done (model: fomo, direct int8 Axon, %ux%u grayscale)", kCaptureWidth,
		kCaptureHeight);

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
