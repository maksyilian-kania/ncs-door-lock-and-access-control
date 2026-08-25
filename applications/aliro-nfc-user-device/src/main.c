/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include <string.h>

#include <hal/nrf_power.h>
#include <nfc_t4t_lib.h>
#include <nrfx.h>
#include <zephyr/device.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/pm/device.h>
#include <zephyr/sys/poweroff.h>

#if !NRF_POWER_HAS_RESETREAS
#include <hal/nrf_reset.h>
#endif

LOG_MODULE_REGISTER(aliro_nfc_ud, LOG_LEVEL_INF);

#define SYSTEM_OFF_DELAY_S 3U
#define APDU_BUFFER_SIZE   1024U

/* Aliro expedited phase AID: A000000909ACCE5501 */
static const uint8_t kAliroExpeditedAid[] = { 0xA0, 0x00, 0x00, 0x09, 0x09, 0xAC, 0xCE, 0x55, 0x01 };

static struct k_work_delayable s_system_off_work;

static uint8_t s_apdu_buffer[APDU_BUFFER_SIZE];
static size_t s_apdu_length;
static bool s_first_message_logged;

static void send_status_response(uint8_t sw1, uint8_t sw2)
{
	const uint8_t response[] = { sw1, sw2 };
	int err = nfc_t4t_response_pdu_send(response, sizeof(response));

	if (err != 0) {
		LOG_ERR("Failed to send R-APDU: %d", err);
	}
}

static bool contains_aliro_expedited_aid(const uint8_t *apdu, size_t length)
{
	if (length < sizeof(kAliroExpeditedAid)) {
		return false;
	}

	for (size_t i = 0; i <= length - sizeof(kAliroExpeditedAid); i++) {
		if (memcmp(&apdu[i], kAliroExpeditedAid, sizeof(kAliroExpeditedAid)) == 0) {
			return true;
		}
	}

	return false;
}

static void log_first_message(const uint8_t *apdu, size_t length)
{
	LOG_HEXDUMP_INF(apdu, length, "First message from Aliro reader (C-APDU)");

	if (length >= 4U) {
		LOG_INF("C-APDU header: CLA=0x%02x INS=0x%02x P1=0x%02x P2=0x%02x", apdu[0], apdu[1],
			apdu[2], apdu[3]);
	}

	if (contains_aliro_expedited_aid(apdu, length)) {
		LOG_INF("Detected Aliro expedited phase SELECT AID");
	} else {
		LOG_INF("Aliro expedited phase AID not found in first message");
	}
}

static void enter_system_off(struct k_work *work)
{
	ARG_UNUSED(work);

	LOG_INF("Entering System OFF — approach an Aliro reader to wake the device");

	if (IS_ENABLED(CONFIG_PM_DEVICE) && IS_ENABLED(CONFIG_SERIAL)) {
		const struct device *const console = DEVICE_DT_GET(DT_CHOSEN(zephyr_console));
		enum pm_device_state state;
		int err;

		if (device_is_ready(console)) {
			do {
				err = pm_device_state_get(console, &state);
			} while ((err == 0) && (state == PM_DEVICE_STATE_ACTIVE));
		}
	}

	sys_poweroff();
}

static void schedule_system_off(void)
{
	k_work_reschedule(&s_system_off_work, K_SECONDS(SYSTEM_OFF_DELAY_S));
}

static void cancel_system_off(void)
{
	k_work_cancel_delayable(&s_system_off_work);
}

static void reset_apdu_assembly(void)
{
	s_apdu_length = 0;
}

static void handle_complete_apdu(void)
{
	if (!s_first_message_logged) {
		log_first_message(s_apdu_buffer, s_apdu_length);
		s_first_message_logged = true;
	}

	/* Minimal placeholder response for the POC — full Aliro handling comes later. */
	send_status_response(0x6A, 0x82);
	reset_apdu_assembly();
}

static void nfc_callback(void *context, nfc_t4t_event_t event, const uint8_t *data, size_t data_length,
			 uint32_t flags)
{
	ARG_UNUSED(context);

	switch (event) {
	case NFC_T4T_EVENT_FIELD_ON:
		cancel_system_off();
		reset_apdu_assembly();
		s_first_message_logged = false;
		LOG_INF("NFC field detected");
		break;

	case NFC_T4T_EVENT_FIELD_OFF:
		schedule_system_off();
		reset_apdu_assembly();
		LOG_INF("NFC field removed");
		break;

	case NFC_T4T_EVENT_DATA_IND:
		if (data_length > 0U) {
			if ((s_apdu_length + data_length) > APDU_BUFFER_SIZE) {
				LOG_ERR("APDU buffer overflow, discarding message");
				reset_apdu_assembly();
				send_status_response(0x6F, 0x00);
				break;
			}

			memcpy(&s_apdu_buffer[s_apdu_length], data, data_length);
			s_apdu_length += data_length;
		}

		if ((flags & NFC_T4T_DI_FLAG_MORE) == 0U) {
			handle_complete_apdu();
		}
		break;

	default:
		break;
	}
}

static void print_reset_reason(void)
{
	uint32_t reason;

#if NRF_POWER_HAS_RESETREAS
	reason = nrf_power_resetreas_get(NRF_POWER);
	nrf_power_resetreas_clear(NRF_POWER, reason);
#else
	reason = nrf_reset_resetreas_get(NRF_RESET);
	nrf_reset_resetreas_clear(NRF_RESET, reason);
#endif

#if NRF_POWER_HAS_RESETREAS
	if (reason & NRF_POWER_RESETREAS_NFC_MASK) {
#else
	if (reason & NRF_RESET_RESETREAS_NFC_MASK) {
#endif
		LOG_INF("Wake-up cause: NFC field detect");
#if NRF_POWER_HAS_RESETREAS
	} else if (reason & NRF_POWER_RESETREAS_RESETPIN_MASK) {
#else
	} else if (reason & NRF_RESET_RESETREAS_RESETPIN_MASK) {
#endif
		LOG_INF("Wake-up cause: pin reset");
#if NRF_POWER_HAS_RESETREAS
	} else if (reason & NRF_POWER_RESETREAS_SREQ_MASK) {
#else
	} else if (reason & NRF_RESET_RESETREAS_SREQ_MASK) {
#endif
		LOG_INF("Wake-up cause: soft reset");
	} else if (reason != 0U) {
		LOG_INF("Wake-up cause: other (0x%08x)", reason);
	} else {
		LOG_INF("Wake-up cause: power-on reset");
	}
}

static int start_nfc_listen_mode(void)
{
	int err;

	/* Raw ISO-DEP mode: do not register an NDEF payload. */
	err = nfc_t4t_setup(nfc_callback, NULL);
	if (err != 0) {
		LOG_ERR("nfc_t4t_setup failed: %d", err);
		return err;
	}

	err = nfc_t4t_emulation_start();
	if (err != 0) {
		LOG_ERR("nfc_t4t_emulation_start failed: %d", err);
		return err;
	}

	LOG_INF("NFC T4T listen mode started (raw ISO-DEP)");
	return 0;
}

int main(void)
{
	LOG_INF("Aliro NFC User Device POC");

	print_reset_reason();

	k_work_init_delayable(&s_system_off_work, enter_system_off);
	schedule_system_off();

	if (start_nfc_listen_mode() != 0) {
		LOG_ERR("NFC initialization failed");
		return 0;
	}

	return 0;
}
