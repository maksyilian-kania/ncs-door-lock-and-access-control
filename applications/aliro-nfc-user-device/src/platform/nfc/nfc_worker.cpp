/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include "platform/nfc/nfc_worker.h"

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <aliro/connection_handle.h>
#include <aliro/types.h>
#include <aliro/user_device/user_device.h>

#include <array>
#include <cerrno>
#include <cstring>

LOG_MODULE_REGISTER(aliro_ud_nfc, CONFIG_ALIRO_UD_NFC_LOG_LEVEL);

namespace AliroUd::Nfc {
namespace {

/*
 * P1 User Device PICS is NFC-only with at most one active session
 * (ncs-aliro stack/src/user_device/session_manager.h kMaxSessions), so
 * every event implicitly targets the one NFC connection handle.
 */
using Aliro::ConnectionHandle;

enum class EventKind : uint8_t {
	FieldOn,
	FieldOff,
	CommandApdu,
	StackEvent,
};

/*
 * Self-contained: each queue entry carries its own copy of the APDU, so a
 * lagging worker thread with several events already queued can never
 * corrupt or race a shared assembly buffer.
 */
struct WorkerEvent {
	EventKind mKind{ EventKind::FieldOn };
	void *mStackEvent{ nullptr };
	size_t mApduLength{ 0 };
	std::array<uint8_t, kMaxApduLength> mApdu{};
};

K_MSGQ_DEFINE(sEventQueue, sizeof(WorkerEvent), CONFIG_ALIRO_UD_NFC_EVENT_QUEUE_DEPTH, alignof(WorkerEvent));

K_THREAD_STACK_DEFINE(sWorkerStackArea, CONFIG_ALIRO_UD_NFC_THREAD_STACK_SIZE);
k_thread sWorkerThread{};

void HandleEvent(const WorkerEvent &event)
{
	const auto handle = ConnectionHandle::Nfc();

	switch (event.mKind) {
	case EventKind::FieldOn: {
		const auto error = Aliro::UserDeviceStack::Instance().ActivateSession(handle);
		if (error != ALIRO_NO_ERROR) {
			LOG_WRN("ActivateSession failed, error: %d", error.ToInt());
		}
		break;
	}
	case EventKind::FieldOff:
		Aliro::UserDeviceStack::Instance().DeactivateSession(handle);
		break;
	case EventKind::CommandApdu: {
		Aliro::Data apdu{ const_cast<uint8_t *>(event.mApdu.data()), event.mApduLength };
		Aliro::UserDeviceStack::Instance().HandleCommandApdu(handle, apdu);
		break;
	}
	case EventKind::StackEvent:
		Aliro::UserDeviceStack::Instance().ProcessEvent(event.mStackEvent);
		break;
	}
}

void WorkerThreadEntry(void *, void *, void *)
{
	WorkerEvent event{};

	while (true) {
		k_msgq_get(&sEventQueue, &event, K_FOREVER);
		HandleEvent(event);
	}
}

int PostEvent(const WorkerEvent &event)
{
	/*
	 * K_NO_WAIT: the producer is either an nfc_t4t_lib callback or a
	 * timer/ISR context (Os::QueueEvent); neither may block. Queue
	 * overflow, stale events, and duplicate field events are handled
	 * deterministically by dropping (logged) rather than blocking or
	 * corrupting state; the stack's own session lifecycle already
	 * tolerates duplicate/stale activation, deactivation, and command
	 * APDU delivery (ALIRO_SESSION_NOT_FOUND is a safe no-op).
	 */
	const int err = k_msgq_put(&sEventQueue, &event, K_NO_WAIT);
	if (err != 0) {
		LOG_ERR("NFC/stack event queue full, dropping event (kind=%d): %d", static_cast<int>(event.mKind),
			err);
	}
	return err;
}

} // namespace

void StartWorker()
{
	k_thread_create(&sWorkerThread, sWorkerStackArea, K_THREAD_STACK_SIZEOF(sWorkerStackArea), WorkerThreadEntry,
			nullptr, nullptr, nullptr, K_PRIO_PREEMPT(CONFIG_ALIRO_UD_NFC_THREAD_PRIORITY), 0, K_NO_WAIT);
	k_thread_name_set(&sWorkerThread, "aliro_ud_nfc");
}

int PostFieldOn()
{
	return PostEvent(WorkerEvent{ EventKind::FieldOn, nullptr, 0, {} });
}

int PostFieldOff()
{
	return PostEvent(WorkerEvent{ EventKind::FieldOff, nullptr, 0, {} });
}

int PostCommandApdu(const uint8_t *apdu, size_t length)
{
	if (length > kMaxApduLength) {
		return -EINVAL;
	}

	WorkerEvent event{ EventKind::CommandApdu, nullptr, length, {} };
	std::memcpy(event.mApdu.data(), apdu, length);

	return PostEvent(event);
}

int PostStackEvent(void *stackEvent)
{
	return PostEvent(WorkerEvent{ EventKind::StackEvent, stackEvent, 0, {} });
}

} // namespace AliroUd::Nfc
