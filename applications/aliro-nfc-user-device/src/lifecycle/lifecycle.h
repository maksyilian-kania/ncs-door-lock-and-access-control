/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#pragma once

#include <aliro/errors.h>

#include "platform/nfc/nfc_worker.h"
#include "platform/power/power.h"

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
 *
 * Also brackets `fn()` with a System OFF mutation guard
 * (docs/system_off_proposal.md): a Button-1 press or auto idle-timeout
 * that becomes due while `fn()` is running never interrupts it and is
 * never dropped - `AliroUd::Power::EndMutationGuard()` fires any such
 * deferred power-off immediately once `fn()` returns.
 */
template <typename Fn> AliroError RunMutation(Fn &&fn)
{
	AliroUd::Nfc::EnterMaintenancePause();

	if (IS_ENABLED(CONFIG_ALIRO_UD_SYSTEM_OFF)) {
		AliroUd::Power::BeginMutationGuard();
	}

	const AliroError result = fn();

	if (IS_ENABLED(CONFIG_ALIRO_UD_SYSTEM_OFF)) {
		AliroUd::Power::EndMutationGuard();
	}

	AliroUd::Nfc::ExitMaintenancePause();
	return result;
}

} // namespace AliroUd::Lifecycle
