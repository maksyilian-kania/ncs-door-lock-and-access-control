/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include <zephyr/logging/log.h>

#include <aliro/user_device/interface.h>

/*
 * Aliro::Interface::UserDevice::Transaction: the stack delivers at most one
 * NotifyResult() per session generation, with an outcome of `Success` (a
 * final EXCHANGE Reader status of success), `Failed`, or `Aborted`. The
 * application's only reaction is logging the coarse, privacy-safe outcome.
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
