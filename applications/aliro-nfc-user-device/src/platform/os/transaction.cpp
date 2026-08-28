/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include <zephyr/logging/log.h>

#include <aliro/user_device/interface.h>

/*
 * Placeholder implementation of Aliro::Interface::UserDevice::Transaction,
 * added only so the application links against the currently checked-out
 * stack: ncs-aliro WP5.5 (decision D8) introduces this contract and calls
 * it unconditionally at the end of every User Device transaction
 * (stack/src/user_device/event_handler.cpp). No application-level reaction
 * to transaction outcomes exists yet (no WP5.5/P1 code path reports
 * `TransactionOutcome::Success`; this only ever observes `Failed`/
 * `Aborted`, per that enum's own documentation) - this stub only logs the
 * coarse, privacy-safe outcome. A later AWP may add real behavior (for
 * example, LED/UX feedback) if APP_PLAN.md is extended to require it.
 */
LOG_MODULE_DECLARE(aliro_ud_stack, CONFIG_NCS_ALIRO_USER_DEVICE_LOG_LEVEL_VALUE);

namespace Aliro::Interface::UserDevice::Transaction {

void NotifyResult(const ::Aliro::UserDevice::TransactionResult &result)
{
	const char *outcome = "unknown";

	switch (result.mOutcome) {
	case ::Aliro::UserDevice::TransactionOutcome::Success:
		outcome = "success";
		break;
	case ::Aliro::UserDevice::TransactionOutcome::Failed:
		outcome = "failed";
		break;
	case ::Aliro::UserDevice::TransactionOutcome::Aborted:
		outcome = "aborted";
		break;
	}

	LOG_INF("Transaction result: outcome=%s generation=%llu", outcome,
		static_cast<unsigned long long>(result.mSessionGeneration));
}

} // namespace Aliro::Interface::UserDevice::Transaction
