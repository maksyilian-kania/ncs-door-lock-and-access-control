/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#pragma once

#include <aliro/errors.h>

#include "platform/nfc/nfc_worker.h"

/**
 * @brief Serializes every mutating credential operation against NFC session
 * activity (APP_PLAN.md AWP3: "mutating commit/delete/factory-reset
 * operations run through a lifecycle coordinator that prevents activation,
 * terminates any active session, applies the storage transaction, and only
 * then resumes normal NFC processing").
 *
 * This is the only module that calls
 * `AliroUd::Credential::Store::Create()/Update()/Delete()/Reset()`; every
 * caller (the CLI) must route those calls through `RunMutation()`.
 */
namespace AliroUd::Lifecycle {

/**
 * @brief Runs `fn` with NFC field activation prevented and any active
 * session terminated first, then resumes normal NFC processing.
 *
 * Blocks the calling thread until the NFC worker thread has acknowledged
 * the pause (deterministically, no polling); `fn` then runs synchronously
 * on the calling thread. Not reentrant: only one mutation may be in flight
 * at a time (true today because CLI commands run sequentially on one shell
 * thread).
 *
 * @param fn Callable with signature `AliroError()`, wrapping exactly one
 * `AliroUd::Credential::Store` mutation.
 *
 * @return Whatever `fn()` returns.
 */
template <typename Fn> AliroError RunMutation(Fn &&fn)
{
	AliroUd::Nfc::EnterMaintenancePause();
	const AliroError result = fn();
	AliroUd::Nfc::ExitMaintenancePause();
	return result;
}

} // namespace AliroUd::Lifecycle
