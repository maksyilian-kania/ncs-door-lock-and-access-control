/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include "platform/nfc/nfc_worker.h"

#include <aliro/user_device/interface.h>

namespace Aliro::Interface::UserDevice::Os {

AliroError QueueEvent(void *event)
{
	/*
	 * Deferred-event delivery is implemented by the single bounded
	 * queue/dedicated thread owned by platform/nfc, so every User Device
	 * stack event and every NFC transport event is serialized through
	 * UserDeviceStack calls on the same thread (APP_PLAN.md AWP1).
	 */
	const int err = AliroUd::Nfc::PostStackEvent(event);
	if (err != 0) {
		return ALIRO_NO_MEMORY;
	}

	return ALIRO_NO_ERROR;
}

} // namespace Aliro::Interface::UserDevice::Os
