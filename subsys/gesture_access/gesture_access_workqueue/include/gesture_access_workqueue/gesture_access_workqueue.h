/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#pragma once

#include <zephyr/kernel.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Submit a work item to the dedicated gesture-access work queue.
 *
 * Used by the SPI camera driver (capture/FIFO-drain) and by the
 * gesture-access feature glue (on-device inference) so that neither ever
 * runs on the system work queue - see CONFIG_DOOR_LOCK_GESTURE_ACCESS_WORKQUEUE.
 *
 * @param work Work item to submit.
 *
 * @return as with k_work_submit_to_queue().
 */
int GestureAccessWorkqueueSubmit(struct k_work *work);

/**
 * @brief Reschedule a delayable work item on the dedicated gesture-access
 * work queue.
 *
 * @param work Delayable work item to schedule.
 * @param delay Delay before the work should run.
 *
 * @return as with k_work_reschedule_for_queue().
 */
int GestureAccessWorkqueueReschedule(struct k_work_delayable *work, k_timeout_t delay);

#ifdef __cplusplus
}
#endif
