/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <aliro/user_device/user_device.h>

LOG_MODULE_REGISTER(aliro_nfc_ud, LOG_LEVEL_INF);

/*
 * main() performs boot sequencing only (see docs/architecture.md). NFC
 * transport, OS bridging, credential/mailbox storage, authorization, and the
 * development CLI are implemented in their own platform/storage/cli modules
 * starting with AWP1; none of that protocol or platform behavior belongs
 * here.
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

	return 0;
}
