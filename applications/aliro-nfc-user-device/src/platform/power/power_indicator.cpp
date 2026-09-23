/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include "power_indicator.h"

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>

/*
 * Real DK LED backend. Hardware-only, following the same pattern as
 * authorization_led.cpp: excluded from host tests, which do not link this
 * file (only the pure platform/power/power_policy.cpp is host-tested; see
 * tests/functional/subsys/aliro_nfc_user_device/power/).
 */
LOG_MODULE_DECLARE(aliro_ud_power, CONFIG_ALIRO_UD_POWER_LOG_LEVEL);

namespace AliroUd::Power::Indicator {

#if DT_HAS_ALIAS(led1)

namespace {
const gpio_dt_spec kLed = GPIO_DT_SPEC_GET(DT_ALIAS(led1), gpios);
bool sReady{ false };
} // namespace

void SetAwake(bool awake)
{
	if (!sReady) {
		if (!device_is_ready(kLed.port)) {
			LOG_ERR("Power indicator LED (led1) GPIO device not ready");
			return;
		}

		if (gpio_pin_configure_dt(&kLed, GPIO_OUTPUT_INACTIVE) != 0) {
			LOG_ERR("Failed to configure power indicator LED (led1) GPIO");
			return;
		}

		sReady = true;
	}

	gpio_pin_set_dt(&kLed, awake ? 1 : 0);
}

#else

void SetAwake(bool awake)
{
	ARG_UNUSED(awake);
	LOG_WRN_ONCE("No 'led1' devicetree alias; the awake indication has no visible backend");
}

#endif

} // namespace AliroUd::Power::Indicator
