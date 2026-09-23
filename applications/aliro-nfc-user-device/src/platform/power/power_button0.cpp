/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include "power_button0.h"

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>

/*
 * Real DK backend. `sw0` is configured (as input, with an edge interrupt)
 * and owned by authorization_button.cpp for its awake role; here we only
 * switch it to a level-sense interrupt right before sys_poweroff() so it can
 * wake the DK from System OFF. On the next boot authorization_button.cpp's
 * SYS_INIT reconfigures it back to the edge interrupt, so there is no
 * persistent conflict.
 */
LOG_MODULE_DECLARE(aliro_ud_power, CONFIG_ALIRO_UD_POWER_LOG_LEVEL);

namespace AliroUd::Power::Button0 {

#if DT_HAS_ALIAS(sw0)

namespace {
const gpio_dt_spec kButton = GPIO_DT_SPEC_GET(DT_ALIAS(sw0), gpios);
} // namespace

void ConfigureWakeSense()
{
	if (!device_is_ready(kButton.port)) {
		LOG_ERR("Button 0 GPIO device not ready; cannot arm System OFF wake");
		return;
	}

	const int err = gpio_pin_interrupt_configure_dt(&kButton, GPIO_INT_LEVEL_ACTIVE);
	if (err != 0) {
		LOG_ERR("Failed to arm Button 0 wake sense: %d", err);
	}
}

#else

void ConfigureWakeSense()
{
	LOG_WRN_ONCE("No 'sw0' devicetree alias; Button 0 has no System OFF wake backend");
}

#endif

} // namespace AliroUd::Power::Button0
