/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include "platform/os/app_status.h"

#include <atomic>

namespace AliroUd::AppStatus {
namespace {

std::atomic<InitState> sInitState{ InitState::NotStarted };

} // namespace

void SetInitState(InitState state)
{
	sInitState.store(state, std::memory_order_release);
}

InitState GetInitState()
{
	return sInitState.load(std::memory_order_acquire);
}

const char *ToString(InitState state)
{
	switch (state) {
	case InitState::NotStarted:
		return "not_started";
	case InitState::StackInitFailed:
		return "stack_init_failed";
	case InitState::NfcStartFailed:
		return "nfc_start_failed";
	case InitState::Running:
		return "running";
	}

	return "unknown";
}

} // namespace AliroUd::AppStatus
