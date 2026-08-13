/*
 * Copyright (c) 2021 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include "app_task.h"

#include <zephyr/logging/log.h>

#ifdef CONFIG_DOOR_LOCK_GESTURE_ACCESS
#include <gesture_access/gesture_access.h>
#endif // CONFIG_DOOR_LOCK_GESTURE_ACCESS

LOG_MODULE_REGISTER(app, CONFIG_CHIP_APP_LOG_LEVEL);

int main()
{
#ifdef CONFIG_DOOR_LOCK_GESTURE_ACCESS

	int gestureAccessErr = DoorLock::GestureAccess::Init();
	if (gestureAccessErr != 0) {
		LOG_ERR("Failed to initialize gesture access: %d", gestureAccessErr);
		return EXIT_FAILURE;
	}

	gestureAccessErr = DoorLock::GestureAccess::Start();
	if (gestureAccessErr != 0) {
		LOG_ERR("Failed to start gesture access: %d", gestureAccessErr);
		return EXIT_FAILURE;
	}

#endif // CONFIG_DOOR_LOCK_GESTURE_ACCESS

	CHIP_ERROR err = AppTask::Instance().StartApp();

	LOG_ERR("Exited with code %" CHIP_ERROR_FORMAT, err.Format());
	return err == CHIP_NO_ERROR ? EXIT_SUCCESS : EXIT_FAILURE;
}
