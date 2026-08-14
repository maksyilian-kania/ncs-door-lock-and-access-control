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

// Work queue API for gesture-access camera capture and on-device gesture inference.

int GestureAccessWorkqueueSubmit(struct k_work *work);

int GestureAccessWorkqueueReschedule(struct k_work_delayable *work, k_timeout_t delay);

#ifdef __cplusplus
}
#endif
