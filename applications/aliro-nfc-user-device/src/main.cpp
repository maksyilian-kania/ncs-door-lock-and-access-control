/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <aliro/user_device/user_device.h>

#include "platform/nfc/nfc_transport.h"

LOG_MODULE_REGISTER(aliro_nfc_ud, LOG_LEVEL_INF);

/*
 * main() performs boot sequencing only (see docs/architecture.md).
 * Credential/mailbox storage, authorization, and the development CLI are
 * implemented in their own storage/cli modules starting with later AWPs;
 * none of that protocol or platform behavior belongs here.
 */
int main(void)
{
	LOG_INF("Aliro NFC User Device");

	const AliroError err = Aliro::UserDeviceStack::Instance().Init();

	if (err != ALIRO_NO_ERROR) {
		/*
		 * AliroError::ToString() is declared in the shared aliro/errors.h
		 * header but its definition (stack/src/errors/errors.cpp) is only
		 * built for CONFIG_NCS_ALIRO (Reader); a User-Device-only build
		 * cannot link it. Use the numeric code instead of narrowing this
		 * skeleton around that gap; see docs/evidence.md.
		 */
		LOG_ERR("User Device stack initialization failed: %d", err.ToInt());
		return 0;
	}

	LOG_INF("User Device stack initialized");

	/*
	 * Starts the dedicated NFC/stack worker thread and NFC-A Type 4
	 * Tag/ISO-DEP listen-mode transport (AWP1). Field activation/removal
	 * and command APDUs are mapped to UserDeviceStack session lifecycle
	 * calls on that worker thread from here on; main() has no further
	 * role to play.
	 */
	if (AliroUd::Nfc::Start() != 0) {
		LOG_ERR("NFC transport initialization failed");
		return 0;
	}

	return 0;
}
