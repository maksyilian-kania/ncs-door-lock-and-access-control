/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include "platform/nfc/nfc_worker.h"

#include "platform/nfc/command_timing.h"

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <aliro/connection_handle.h>
#include <aliro/types.h>
#include <aliro/user_device/user_device.h>

#include <array>
#include <atomic>
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
	CommandApdu,
	StackEvent,
};

/*
 * Self-contained: each queue entry carries its own copy of the APDU, so a
 * lagging worker thread with several events already queued can never
 * corrupt or race a shared assembly buffer.
 */
struct WorkerEvent {
	EventKind mKind{ EventKind::CommandApdu };
	void *mStackEvent{ nullptr };
	size_t mApduLength{ 0 };
	std::array<uint8_t, kMaxApduLength> mApdu{};
};

K_MSGQ_DEFINE(sEventQueue, sizeof(WorkerEvent), CONFIG_ALIRO_UD_NFC_EVENT_QUEUE_DEPTH, alignof(WorkerEvent));

/*
 * Field lifecycle transitions never share the bounded queue above:
 * FIELD_ON/FIELD_OFF are coalesced into this single pending intent instead
 * of being queued, so they can never be dropped for lack of queue space
 * (issue: "Stop silently dropping lifecycle events"). Only the latest
 * intent matters because the stack already treats a repeated
 * activation/SELECT as restarting the logical Aliro transaction.
 *
 * `sForceRecoveryRequested` is set when a CommandApdu/StackEvent could not
 * be queued (queue full): rather than silently continuing on skipped
 * data, the worker deterministically tears down any active session on its
 * next wake.
 *
 * Both are only ever written from producer contexts (nfc_t4t_lib
 * callback, ISR/timer) and only ever read/cleared on the worker thread.
 */
enum class FieldIntent : uint8_t { None, On, Off };

std::atomic<FieldIntent> sPendingField{ FieldIntent::None };
std::atomic<bool> sForceRecoveryRequested{ false };
K_SEM_DEFINE(sLifecycleSem, 0, 1);

/*
 * Maintenance pause (APP_PLAN.md AWP3 lifecycle coordinator): while
 * `sActivationAllowed` is false, HandleFieldOn() below never calls
 * ActivateSession(), no matter how the worker's drain loop happens to
 * interleave a pending FIELD_ON with the pause request itself. Checked
 * from any thread by EnterMaintenancePause(); only ever written from
 * EnterMaintenancePause()/ExitMaintenancePause() (never concurrently, by
 * construction: RunMutation() serializes mutations).
 */
std::atomic<bool> sActivationAllowed{ true };
std::atomic<bool> sPauseRequested{ false };
K_SEM_DEFINE(sPauseAck, 0, 1);

K_THREAD_STACK_DEFINE(sWorkerStackArea, CONFIG_ALIRO_UD_NFC_THREAD_STACK_SIZE);
k_thread sWorkerThread{};

/*
 * Worker-thread-only state: every read/write below happens exclusively on
 * the dedicated NFC/stack worker thread (HandleFieldOn/Off(),
 * HandleForcedRecovery(), HandleEvent(), and NotifySessionTerminated(),
 * which is always invoked synchronously from a call already running on
 * this thread - see nfc_worker.h). No additional synchronization is
 * needed for these variables.
 */
/*
 * std::atomic only for visibility to diagnostic readers on other threads
 * (IsSessionActive(), tests); every write still happens exclusively on the
 * worker thread, so this is a single-writer/multi-reader variable, not a
 * synchronization point for session-state mutation itself.
 */
std::atomic<bool> sSessionActive{ false };
bool sExpectingOwnTermination{ false };
std::atomic<size_t> sActivationAttempts{ 0 };
std::atomic<size_t> sRejectedApduCount{ 0 };

void HandleFieldOn()
{
	const auto handle = ConnectionHandle::Nfc();

	if (sSessionActive) {
		LOG_INF("FIELD_ON: already active, ignored");
		return;
	}

	if (!sActivationAllowed.load(std::memory_order_relaxed)) {
		LOG_INF("FIELD_ON ignored: NFC activation paused for maintenance");
		return;
	}

	sActivationAttempts.fetch_add(1, std::memory_order_relaxed);

	const auto error = Aliro::UserDeviceStack::Instance().ActivateSession(handle);
	if (error != ALIRO_NO_ERROR) {
		LOG_WRN("ActivateSession failed, error: %d", error.ToInt());
		return;
	}

	sSessionActive = true;
	LOG_INF("FIELD_ON: inactive -> active");
}

void HandleFieldOff()
{
	const auto handle = ConnectionHandle::Nfc();

	if (!sSessionActive) {
		LOG_INF("FIELD_OFF: already inactive, ignored");
		return;
	}

	/*
	 * DeactivateSession() synchronously invokes
	 * Aliro::Interface::UserDevice::Nfc::HandleTermination() ->
	 * NotifySessionTerminated(); this flag lets that call know the
	 * transition is the expected result of this FIELD_OFF (logged here)
	 * rather than an independent stack-driven termination.
	 */
	sExpectingOwnTermination = true;
	Aliro::UserDeviceStack::Instance().DeactivateSession(handle);
	sExpectingOwnTermination = false;
	sSessionActive = false;
	LOG_INF("FIELD_OFF: active -> inactive");
}

void HandleForcedRecovery()
{
	if (!sSessionActive) {
		LOG_WRN("Forced session recovery requested (event queue overflow), no active session");
		return;
	}

	LOG_ERR("Forced session recovery: tearing down active session after event queue overflow");

	const auto handle = ConnectionHandle::Nfc();
	sExpectingOwnTermination = true;
	Aliro::UserDeviceStack::Instance().DeactivateSession(handle);
	sExpectingOwnTermination = false;
	sSessionActive = false;
	LOG_INF("STACK_TERMINATION: active -> inactive (forced recovery)");
}

/*
 * Terminates any active session (if one exists) and acknowledges the pause
 * request; HandleFieldOn()'s own sActivationAllowed check is what actually
 * prevents a still-pending/racing FIELD_ON from reactivating one, not the
 * ordering of this function relative to DrainPendingLifecycle()'s other
 * branches.
 */
void HandlePauseRequest()
{
	if (sSessionActive) {
		LOG_INF("Maintenance pause: terminating active session");
		const auto handle = ConnectionHandle::Nfc();
		sExpectingOwnTermination = true;
		Aliro::UserDeviceStack::Instance().DeactivateSession(handle);
		sExpectingOwnTermination = false;
		sSessionActive = false;
	}

	k_sem_give(&sPauseAck);
}

/** @brief Drains the coalesced lifecycle channel: pause request, then forced recovery, then the latest field intent. */
void DrainPendingLifecycle()
{
	if (sPauseRequested.exchange(false, std::memory_order_relaxed)) {
		HandlePauseRequest();
	}

	if (sForceRecoveryRequested.exchange(false, std::memory_order_relaxed)) {
		HandleForcedRecovery();
	}

	switch (sPendingField.exchange(FieldIntent::None, std::memory_order_relaxed)) {
	case FieldIntent::None:
		break;
	case FieldIntent::On:
		HandleFieldOn();
		break;
	case FieldIntent::Off:
		HandleFieldOff();
		break;
	}
}

void HandleEvent(const WorkerEvent &event)
{
	const auto handle = ConnectionHandle::Nfc();

	switch (event.mKind) {
	case EventKind::CommandApdu: {
		if (!sSessionActive) {
			sRejectedApduCount.fetch_add(1, std::memory_order_relaxed);
			LOG_WRN("Dropping command APDU (%zu bytes): no active session", event.mApduLength);
			break;
		}

		Aliro::Data apdu{ const_cast<uint8_t *>(event.mApdu.data()), event.mApduLength };
		/*
		 * HandleCommandApdu() only returns after the stack has
		 * already called
		 * Aliro::Interface::UserDevice::Nfc::SendResponseApdu()
		 * synchronously (nfc_transport.cpp), so this pair measures
		 * exactly the command-delivery-to-response-sent boundary
		 * (APP_PLAN.md AWP7; see command_timing.h).
		 */
		BeginCommandTiming();
		Aliro::UserDeviceStack::Instance().HandleCommandApdu(handle, apdu);
		EndCommandTiming();
		break;
	}
	case EventKind::StackEvent:
		Aliro::UserDeviceStack::Instance().ProcessEvent(event.mStackEvent);
		break;
	}
}

void RequestForcedRecovery(EventKind kind, int postError)
{
	LOG_ERR("NFC/stack event queue full, dropping event (kind=%d): %d; forcing session recovery",
		static_cast<int>(kind), postError);
	sForceRecoveryRequested.store(true, std::memory_order_relaxed);
	k_sem_give(&sLifecycleSem);
}

int PostEvent(const WorkerEvent &event)
{
	/*
	 * K_NO_WAIT: the producer is either an nfc_t4t_lib callback or a
	 * timer/ISR context (Os::QueueEvent); neither may block. On overflow,
	 * dropping this single message is deterministic and, unlike before,
	 * never leaves stale session state behind: RequestForcedRecovery()
	 * guarantees the current session (if any) is torn down on the
	 * worker's next wake rather than silently continuing.
	 */
	const int err = k_msgq_put(&sEventQueue, &event, K_NO_WAIT);
	if (err != 0) {
		RequestForcedRecovery(event.mKind, err);
	}
	return err;
}

void WorkerThreadEntry(void *, void *, void *)
{
	while (true) {
		k_poll_event events[2]{
			K_POLL_EVENT_INITIALIZER(K_POLL_TYPE_SEM_AVAILABLE, K_POLL_MODE_NOTIFY_ONLY, &sLifecycleSem),
			K_POLL_EVENT_INITIALIZER(K_POLL_TYPE_MSGQ_DATA_AVAILABLE, K_POLL_MODE_NOTIFY_ONLY, &sEventQueue),
		};

		k_poll(events, ARRAY_SIZE(events), K_FOREVER);

		if (events[0].state == K_POLL_STATE_SEM_AVAILABLE) {
			k_sem_take(&sLifecycleSem, K_NO_WAIT);
			DrainPendingLifecycle();
		}

		if (events[1].state == K_POLL_STATE_MSGQ_DATA_AVAILABLE) {
			WorkerEvent event{};
			if (k_msgq_get(&sEventQueue, &event, K_NO_WAIT) == 0) {
				HandleEvent(event);
			}
		}
	}
}

} // namespace

void StartWorker()
{
	k_thread_create(&sWorkerThread, sWorkerStackArea, K_THREAD_STACK_SIZEOF(sWorkerStackArea), WorkerThreadEntry,
			nullptr, nullptr, nullptr, K_PRIO_PREEMPT(CONFIG_ALIRO_UD_NFC_THREAD_PRIORITY), 0, K_NO_WAIT);
	k_thread_name_set(&sWorkerThread, "aliro_ud_nfc");
}

void PostFieldOn()
{
	sPendingField.store(FieldIntent::On, std::memory_order_relaxed);
	k_sem_give(&sLifecycleSem);
}

void PostFieldOff()
{
	sPendingField.store(FieldIntent::Off, std::memory_order_relaxed);
	k_sem_give(&sLifecycleSem);
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

void NotifySessionTerminated()
{
	sSessionActive = false;

	if (!sExpectingOwnTermination) {
		/*
		 * Independent stack-driven termination (watchdog timeout,
		 * protocol failure): the FIELD_OFF/forced-recovery paths
		 * already log their own transition line when they are the
		 * ones that triggered this call.
		 */
		LOG_INF("STACK_TERMINATION: active -> inactive");
	}
}

void EnterMaintenancePause()
{
	sActivationAllowed.store(false, std::memory_order_relaxed);
	sPauseRequested.store(true, std::memory_order_relaxed);
	k_sem_give(&sLifecycleSem);
	k_sem_take(&sPauseAck, K_FOREVER);
}

void ExitMaintenancePause()
{
	sActivationAllowed.store(true, std::memory_order_relaxed);
}

bool IsSessionActive()
{
	return sSessionActive;
}

size_t GetActivationAttemptCount()
{
	return sActivationAttempts.load(std::memory_order_relaxed);
}

size_t GetRejectedApduCount()
{
	return sRejectedApduCount.load(std::memory_order_relaxed);
}

} // namespace AliroUd::Nfc
