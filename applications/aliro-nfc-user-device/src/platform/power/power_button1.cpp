/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include "power_button1.h"
#include "power.h"

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

/*
 * Real DK button backend for the Button 1 awake-only hold/off toggle,
 * following the same ISR-submits-a-work-item pattern as
 * authorization_button.cpp (see that file's header comment for why the real
 * work happens on the system workqueue thread rather than in ISR context).
 * Button 1 is deliberately not a System OFF wake source (Button 0 is); it is
 * only meaningful while the DK is already awake.
 */
LOG_MODULE_DECLARE(aliro_ud_power, CONFIG_ALIRO_UD_POWER_LOG_LEVEL);

namespace AliroUd::Power::Button1 {

#if DT_HAS_ALIAS(sw1)

namespace {

const gpio_dt_spec kButton = GPIO_DT_SPEC_GET(DT_ALIAS(sw1), gpios);
gpio_callback sCallback{};
k_work sPressWork{};
k_work_delayable sArmWork{};

void HandlePressWork(k_work *)
{
	/*
	 * Defensive re-check: only act if sw1 is actually physically active
	 * right now. Guards against a spurious callback invocation being
	 * misattributed to this pin (observed on-target: pressing Button 0
	 * alone triggered this handler - see docs/system_off_proposal.md
	 * caveat #1 on GPIO wake-source/LATCH attribution on this chip
	 * family). Cheap to check here since we are already off the ISR.
	 */
	if (gpio_pin_get_dt(&kButton) != 1) {
		return;
	}

	AliroUd::Power::HandleButton1Press();
}

void OnPressed(const device *, gpio_callback *, uint32_t pins)
{
	ARG_UNUSED(pins);
	k_work_submit(&sPressWork);
}

/**
 * @brief Arms the edge toggle interrupt after the settle delay. Not done
 * directly in Init(): see the settle-delay rationale on power_button1.h's
 * Init() declaration.
 */
void ArmEdgeInterrupt(k_work *)
{
	const int err = gpio_pin_interrupt_configure_dt(&kButton, GPIO_INT_EDGE_TO_ACTIVE);
	if (err != 0) {
		LOG_ERR("Failed to arm Button 1 toggle interrupt: %d", err);
	}
}

} // namespace

void Init()
{
	if (!device_is_ready(kButton.port)) {
		LOG_ERR("Button 1 GPIO device not ready");
		return;
	}

	k_work_init(&sPressWork, HandlePressWork);
	k_work_init_delayable(&sArmWork, ArmEdgeInterrupt);

	int err = gpio_pin_configure_dt(&kButton, GPIO_INPUT);
	if (err != 0) {
		LOG_ERR("Failed to configure Button 1 GPIO: %d", err);
		return;
	}

	gpio_init_callback(&sCallback, OnPressed, BIT(kButton.pin));

	err = gpio_add_callback(kButton.port, &sCallback);
	if (err != 0) {
		LOG_ERR("Failed to add Button 1 callback: %d", err);
		return;
	}

	k_work_schedule(&sArmWork, K_MSEC(CONFIG_ALIRO_UD_SYSTEM_OFF_BUTTON1_SETTLE_MS));
}

#else

void Init()
{
	LOG_WRN("No 'sw1' devicetree alias; Button 1 hold-awake/off toggle has no hardware backend");
}

#endif

} // namespace AliroUd::Power::Button1
