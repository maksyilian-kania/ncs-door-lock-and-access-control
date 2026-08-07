/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include <gesture_access_workqueue/gesture_access_workqueue.h>

#include <zephyr/init.h>
#include <zephyr/kernel.h>

/*
 * This work queue is intentionally isolated from CONFIG_SYSTEM_WORKQUEUE
 * (k_sys_work_q), mirroring subsys/aliro/aliro_workqueue: this app has hard
 * real-time obligations (BLE/Matter/Aliro/NFC/UWB) and camera capture, SPI
 * FIFO drains, and on-device gesture inference are comparatively long-running
 * and bursty. Running them on the system work queue would risk delaying
 * unrelated, latency-sensitive work items queued by other subsystems (and
 * vice versa). Do not "simplify" this back onto k_sys_work_q - see the
 * real-time/scheduling analysis in the camera driver plan for details.
 */

namespace {

k_work_q sGestureAccessWorkQ;
K_THREAD_STACK_DEFINE(sGestureAccessWorkQStack, CONFIG_DOOR_LOCK_GESTURE_ACCESS_WORKQUEUE_STACK_SIZE);

int GestureAccessWorkqueueInit(void)
{
	constexpr k_work_queue_config config{
		.name = "gestaccessworkq",
		.no_yield = false,
		.essential = true,
		.work_timeout_ms = 0,
	};

	k_work_queue_start(&sGestureAccessWorkQ, sGestureAccessWorkQStack,
			   K_THREAD_STACK_SIZEOF(sGestureAccessWorkQStack),
			   CONFIG_DOOR_LOCK_GESTURE_ACCESS_WORKQUEUE_PRIORITY, &config);
	return 0;
}

SYS_INIT(GestureAccessWorkqueueInit, APPLICATION,
	 CONFIG_DOOR_LOCK_GESTURE_ACCESS_WORKQUEUE_INIT_PRIORITY);

} // namespace

extern "C" {

int GestureAccessWorkqueueSubmit(struct k_work *work)
{
	return k_work_submit_to_queue(&sGestureAccessWorkQ, work);
}

int GestureAccessWorkqueueReschedule(struct k_work_delayable *work, k_timeout_t delay)
{
	return k_work_reschedule_for_queue(&sGestureAccessWorkQ, work, delay);
}

} // extern "C"
