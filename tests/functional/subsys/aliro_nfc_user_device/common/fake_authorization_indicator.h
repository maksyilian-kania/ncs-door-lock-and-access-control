/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#pragma once

#include <cstddef>

/**
 * @brief Recording fake for `authorization_indicator.h` (APP_PLAN.md AWP4
 * host-test support): substitutes for the real DK LED backend
 * (`authorization_led.cpp`), which native_sim cannot build (no `led0`
 * devicetree alias).
 */
namespace AliroUd::Authorization::Test {

/** @brief Clears every recorded call. Call at test start. */
void ResetFakeAuthorizationIndicator();

/** @brief Number of `Indicator::SetActive()` calls made so far. */
size_t GetSetActiveCallCount();

/** @brief The `active` argument of the most recent `Indicator::SetActive()` call. */
bool GetLastActive();

} // namespace AliroUd::Authorization::Test
