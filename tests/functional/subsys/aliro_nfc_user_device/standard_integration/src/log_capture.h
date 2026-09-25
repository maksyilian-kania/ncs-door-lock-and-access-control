/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Test log backend that records each formatted log line containing
 * "Transaction result:", the only observable behavior of the application's
 * `Aliro::Interface::UserDevice::Transaction::NotifyResult()` adapter.
 * Requires CONFIG_LOG_MODE_IMMEDIATE.
 */
void log_capture_reset(void);

/** @brief Number of recorded lines containing `needle`. */
size_t log_capture_count(const char *needle);

/** @brief Total number of recorded lines. */
size_t log_capture_total(void);

#ifdef __cplusplus
}
#endif
