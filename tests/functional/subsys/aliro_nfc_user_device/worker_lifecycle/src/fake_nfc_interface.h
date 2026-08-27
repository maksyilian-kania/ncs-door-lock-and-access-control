/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

/**
 * @brief Test double for `Aliro::Interface::UserDevice::Nfc`.
 *
 * Substitutes for the real, hardware-dependent `nfc_transport.cpp` so that
 * `nfc_worker.cpp`, the `platform/os` bindings, and the real
 * `Aliro::UserDeviceStack` can be exercised end to end on
 * native_sim/native/64 (AWP1).
 */
namespace AliroUdTest::FakeNfc {

/** @brief Resets every counter/capture below. Call between test cases. */
void Reset();

/** @brief Number of `SendResponseApdu()` calls since the last `Reset()`. */
size_t GetSentResponseCount();

/** @brief The response APDU bytes passed to the most recent `SendResponseApdu()` call. */
std::vector<uint8_t> GetLastResponse();

/** @brief Number of `HandleTermination()` calls since the last `Reset()`. */
size_t GetTerminationCount();

} // namespace AliroUdTest::FakeNfc
