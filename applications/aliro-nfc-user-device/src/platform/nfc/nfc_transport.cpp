/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include "platform/nfc/nfc_transport.h"
#include "platform/nfc/apdu_fragment_assembler.h"
#include "platform/nfc/nfc_worker.h"

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <nfc_t4t_lib.h>

#include <aliro/user_device/interface.h>

#include <array>
#include <cstring>

LOG_MODULE_DECLARE(aliro_ud_nfc, CONFIG_ALIRO_UD_NFC_LOG_LEVEL);

namespace AliroUd::Nfc {

/*
 * Raw ISO-DEP fragment reassembly state (APP_PLAN.md AWP1: "Keep transport
 * fragment assembly in this module. Do not parse Aliro APDUs."). Owned
 * exclusively by this translation unit, but touched from two execution
 * contexts: the nfc_t4t_lib callback (HandleDataIndication(), and
 * FIELD_ON/FIELD_OFF in NfcCallback()) and the NFC/stack worker thread
 * (ResetAssembly(), via HandleTermination() below). `sAssemblerLock`
 * serializes every access; neither critical section blocks/sleeps, so a
 * spinlock is sufficient and avoids priority-inversion/scheduling concerns.
 * The assembly logic itself lives in the host-testable
 * ApduFragmentAssembler; only the nfc_t4t_lib glue (this file) is
 * hardware-dependent.
 */
static ApduFragmentAssembler sAssembler{};
static struct k_spinlock sAssemblerLock{};

/*
 * The response transmit buffer. `SendResponseApdu()` copies into it and
 * retains it unchanged until the next nfc_t4t_lib callback
 * (NFC_T4T_EVENT_DATA_TRANSMITTED, NFC_T4T_EVENT_DATA_IND, or
 * NFC_T4T_EVENT_FIELD_OFF), per APP_PLAN.md AWP1. `SendResponseApdu()` only
 * ever runs on the worker thread (called synchronously from
 * UserDeviceStack::HandleCommandApdu(), itself only ever called from the
 * worker thread), so no additional synchronization is needed here either.
 */
static std::array<uint8_t, kMaxApduLength> sTransmitBuffer{};

static void ResetAssembly()
{
	const k_spinlock_key_t key = k_spin_lock(&sAssemblerLock);
	sAssembler.Reset();
	k_spin_unlock(&sAssemblerLock, key);
}

namespace {

void HandleDataIndication(const uint8_t *data, size_t dataLength, uint32_t flags)
{
	const bool more = (flags & NFC_T4T_DI_FLAG_MORE) != 0U;

	const k_spinlock_key_t key = k_spin_lock(&sAssemblerLock);
	const auto result = sAssembler.AddFragment(data, dataLength, more);

	switch (result) {
	case ApduFragmentAssembler::Result::Incomplete:
		/* Chained fragment: wait for the rest before posting a command. */
		k_spin_unlock(&sAssemblerLock, key);
		return;

	case ApduFragmentAssembler::Result::Overflow: {
		k_spin_unlock(&sAssemblerLock, key);

		LOG_ERR("Command APDU fragment assembly overflow, discarding message");

		/*
		 * Transport-layer framing bound (more raw ISO-DEP fragment bytes
		 * than any valid short-length command APDU can hold), not an
		 * Aliro protocol failure: responded to directly here, with a
		 * generic ISO/IEC 7816-4 status word, rather than going through
		 * the stack, so this module stays free of Aliro APDU/TLV parsing
		 * (APP_PLAN.md AWP1).
		 */
		static constexpr std::array<uint8_t, 2> kFramingErrorResponse{ 0x6F, 0x00 };
		const int err =
			nfc_t4t_response_pdu_send(kFramingErrorResponse.data(), kFramingErrorResponse.size());
		if (err != 0) {
			LOG_ERR("Failed to send framing-error R-APDU: %d", err);
		}
		return;
	}

	case ApduFragmentAssembler::Result::Complete: {
		/*
		 * Copy the assembled bytes out and reset while still holding the
		 * lock, so a concurrent ResetAssembly() from the worker thread
		 * (HandleTermination()) can never observe/corrupt a half-copied
		 * buffer. PostCommandApdu() only does a bounded memcpy and a
		 * non-blocking k_msgq_put(), so calling it under the spinlock is
		 * safe (no sleep/scheduling call).
		 */
		const auto assembled = sAssembler.GetAssembled();
		const int err = PostCommandApdu(assembled.data(), assembled.size());
		sAssembler.Reset();
		k_spin_unlock(&sAssemblerLock, key);

		if (err != 0) {
			LOG_ERR("Failed to post assembled command APDU, error: %d", err);
		}
		return;
	}
	}
}

void NfcCallback(void *context, nfc_t4t_event_t event, const uint8_t *data, size_t dataLength, uint32_t flags)
{
	ARG_UNUSED(context);

	switch (event) {
	case NFC_T4T_EVENT_FIELD_ON:
		ResetAssembly();
		PostFieldOn();
		break;

	case NFC_T4T_EVENT_FIELD_OFF:
		ResetAssembly();
		PostFieldOff();
		break;

	case NFC_T4T_EVENT_DATA_IND:
		HandleDataIndication(data, dataLength, flags);
		break;

	default:
		break;
	}
}

} // namespace

int Start()
{
	StartWorker();

	int err = nfc_t4t_setup(NfcCallback, nullptr);
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

} // namespace AliroUd::Nfc

namespace Aliro::Interface::UserDevice::Nfc {

AliroError SendResponseApdu(ConnectionHandle handle, ConstData apdu)
{
	ARG_UNUSED(handle);

	if (apdu.mLength > AliroUd::Nfc::sTransmitBuffer.size()) {
		return ALIRO_NO_MEMORY;
	}

	std::memcpy(AliroUd::Nfc::sTransmitBuffer.data(), apdu.mData, apdu.mLength);

	const int err = nfc_t4t_response_pdu_send(AliroUd::Nfc::sTransmitBuffer.data(), apdu.mLength);
	if (err != 0) {
		return ALIRO_ERROR_INTERNAL;
	}

	return ALIRO_NO_ERROR;
}

void HandleTermination(ConnectionHandle handle)
{
	ARG_UNUSED(handle);
	AliroUd::Nfc::ResetAssembly();
	AliroUd::Nfc::NotifySessionTerminated();
}

TimingConstraints GetTimingConstraints(ConnectionHandle handle)
{
	ARG_UNUSED(handle);

	/*
	 * AWP7 (APP_PLAN.md) searched the Aliro 1.0 Specification and Test
	 * Plan corpus for a normative, numeric NFC command-processing-time
	 * bound applicable to this application's PICS and found none: the
	 * only explicit application-layer timeout in that corpus (1500 ms)
	 * applies to the BLE interface, which this NFC-only User Device does
	 * not implement. The transport-level ISO-DEP Frame Waiting Time is
	 * negotiated by `nfc_t4t_lib`/`isodep.c` itself, is not exposed to
	 * this application, and this application has no API to request a
	 * Waiting Time Extension when it needs more of that budget; there is
	 * therefore no value this function could report that the caller
	 * could act on. `command_timing.h`'s command-to-response duration
	 * measurements are AWP7's informational/regression evidence for
	 * ALIRO-UD-SYRS-P1-040 instead (see docs/evidence.md).
	 */
	return TimingConstraints{};
}

} // namespace Aliro::Interface::UserDevice::Nfc
