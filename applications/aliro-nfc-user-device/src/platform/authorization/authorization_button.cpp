/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include "authorization_indicator.h"
#include "authorization_window.h"

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

/*
 * Real DK button backend: a press opens AliroUd::Authorization::GlobalWindow()
 * for CONFIG_ALIRO_UD_AUTHORIZATION_WINDOW_SECONDS and clears the visible
 * indication (APP_PLAN.md AWP4). Hardware-only: excluded from host tests
 * (native_sim has no `sw0` devicetree alias); host tests call
 * AliroUd::Authorization::GlobalWindow().Open() directly (or drive it
 * through the "aliro-ud auth press" CLI test trigger), the same role this
 * ISR plays on target.
 *
 * `gpio_add_callback()` invokes the callback from GPIO ISR context. The
 * callback below therefore only submits a work item: everything else
 * (`Window::Open()`, `Indicator::SetActive()` including its lazy
 * first-call `gpio_pin_configure_dt()`, and `LOG_INF()` under
 * `CONFIG_LOG_MODE_IMMEDIATE=y`, which blocks on the console UART) runs on
 * the system workqueue thread instead. Doing that work directly in the ISR
 * measurably overflowed the interrupt stack on-target (`K_ERR_STACK_CHK_FAIL`,
 * caught by the Armv8-M stack-limit registers) even though
 * `CONFIG_ISR_STACK_SIZE` is unchanged from the SoC default; see
 * docs/evidence.md AWP4 for the on-target crash this fixes.
 */
LOG_MODULE_DECLARE(aliro_ud_authorization, CONFIG_ALIRO_UD_AUTHORIZATION_LOG_LEVEL);

#if DT_HAS_ALIAS(sw0)

namespace {

const gpio_dt_spec kButton = GPIO_DT_SPEC_GET(DT_ALIAS(sw0), gpios);
gpio_callback sButtonCallback{};
k_work sButtonWork{};

void HandleButtonPressWork(k_work *)
{
	AliroUd::Authorization::GlobalWindow().Open(
		k_uptime_get(), static_cast<uint32_t>(CONFIG_ALIRO_UD_AUTHORIZATION_WINDOW_SECONDS) * 1000U);
	AliroUd::Authorization::Indicator::SetActive(false);
	LOG_INF("Button pressed: authorization window opened for %d s", CONFIG_ALIRO_UD_AUTHORIZATION_WINDOW_SECONDS);
}

void OnButtonPressed(const device *, gpio_callback *, uint32_t)
{
	k_work_submit(&sButtonWork);
}

int InitAuthorizationButton()
{
	if (!device_is_ready(kButton.port)) {
		LOG_ERR("Authorization button GPIO device not ready");
		return -ENODEV;
	}

	k_work_init(&sButtonWork, HandleButtonPressWork);

	int err = gpio_pin_configure_dt(&kButton, GPIO_INPUT);
	if (err != 0) {
		LOG_ERR("Failed to configure authorization button GPIO: %d", err);
		return err;
	}

	err = gpio_pin_interrupt_configure_dt(&kButton, GPIO_INT_EDGE_TO_ACTIVE);
	if (err != 0) {
		LOG_ERR("Failed to configure authorization button interrupt: %d", err);
		return err;
	}

	gpio_init_callback(&sButtonCallback, OnButtonPressed, BIT(kButton.pin));

	err = gpio_add_callback(kButton.port, &sButtonCallback);
	if (err != 0) {
		LOG_ERR("Failed to add authorization button callback: %d", err);
		return err;
	}

	return 0;
}

SYS_INIT(InitAuthorizationButton, APPLICATION, CONFIG_KERNEL_INIT_PRIORITY_DEFAULT);

} // namespace

#else

namespace {
int WarnNoButton()
{
	LOG_WRN("No 'sw0' devicetree alias; the authorization window can only be opened through the "
		"'aliro-ud auth press' CLI test trigger");
	return 0;
}

SYS_INIT(WarnNoButton, APPLICATION, CONFIG_KERNEL_INIT_PRIORITY_DEFAULT);
} // namespace

#endif
