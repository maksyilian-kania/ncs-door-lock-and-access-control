/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#pragma once

#include <cstdint>

/**
 * @brief Non-secret, whole-application boot/initialization status.
 *
 * `main.cpp` is the only writer (boot is single-threaded up to this point);
 * any thread may read it afterwards. Used by the development CLI's `info`
 * command (APP_PLAN.md AWP2) to report initialization state without any
 * module reaching into `main.cpp` internals.
 */
namespace AliroUd::AppStatus {

enum class InitState : uint8_t {
	/** `main()` has not finished boot sequencing yet. */
	NotStarted,
	/** `Aliro::UserDeviceStack::Instance().Init()` returned an error. */
	StackInitFailed,
	/** `AliroUd::Nfc::Start()` returned an error. */
	NfcStartFailed,
	/** Boot sequencing completed; the NFC/stack worker thread is running. */
	Running,
};

/** @brief Records the outcome of one boot sequencing step. Called only from `main()`. */
void SetInitState(InitState state);

/** @brief Current initialization state; safe to call from any thread. */
InitState GetInitState();

/** @brief Short, stable, machine-readable token for `state` (for example "running"). */
const char *ToString(InitState state);

} // namespace AliroUd::AppStatus
