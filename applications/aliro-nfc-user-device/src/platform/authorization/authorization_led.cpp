/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include "authorization_indicator.h"

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>

/*
 * Real DK LED backend for Aliro::Interface::UserDevice::Authorization's
 * visible indication (APP_PLAN.md AWP4). Hardware-only: excluded from host
 * tests (native_sim has no `led0` devicetree alias), which link
 * fake_authorization_indicator.cpp instead - the same split used for
 * nfc_transport.cpp/fake_nfc_interface.cpp.
 */
LOG_MODULE_DECLARE(aliro_ud_authorization, CONFIG_ALIRO_UD_AUTHORIZATION_LOG_LEVEL);

namespace AliroUd::Authorization::Indicator {

#if DT_HAS_ALIAS(led0)

namespace {
const gpio_dt_spec kLed = GPIO_DT_SPEC_GET(DT_ALIAS(led0), gpios);
bool sReady{ false };
} // namespace

void SetActive(bool active)
{
	if (!sReady) {
		if (!device_is_ready(kLed.port)) {
			LOG_ERR("Authorization LED GPIO device not ready");
			return;
		}

		if (gpio_pin_configure_dt(&kLed, GPIO_OUTPUT_INACTIVE) != 0) {
			LOG_ERR("Failed to configure authorization LED GPIO");
			return;
		}

		sReady = true;
	}

	gpio_pin_set_dt(&kLed, active ? 1 : 0);
}

#else

void SetActive(bool active)
{
	ARG_UNUSED(active);
	LOG_WRN_ONCE("No 'led0' devicetree alias; authorization indication has no visible backend");
}

#endif

} // namespace AliroUd::Authorization::Indicator
