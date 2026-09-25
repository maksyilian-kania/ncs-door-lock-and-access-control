/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <aliro/user_device/interface.h>

#include <array>

LOG_MODULE_DECLARE(aliro_ud_os, CONFIG_ALIRO_UD_OS_LOG_LEVEL);

namespace Aliro::Interface::UserDevice::Os::Timer {
namespace {

struct Slot {
	bool mInitialized{ false };
	bool mAcquired{ false };
	Callback mCallback{ nullptr };
	void *mContext{ nullptr };
	k_timer mTimer{};
	k_work mWork{};
};

std::array<Slot, CONFIG_ALIRO_UD_OS_MAX_TIMERS> gSlots{};

/* Guards mAcquired/mCallback/mContext against the expiry ISR and the work handler. */
k_spinlock gLock{};

/*
 * Stack callbacks take the stack mutex (for example
 * Aliro::UserDevice::UserDeviceSession::WatchdogExpiredCallback()), so they
 * must run in thread context: the k_timer expiry ISR only submits this
 * slot's work item to a dedicated work queue.
 */
K_THREAD_STACK_DEFINE(gWorkQueueStack, CONFIG_ALIRO_UD_OS_TIMER_THREAD_STACK_SIZE);
k_work_q gWorkQueue{};

void WorkHandler(k_work *work)
{
	auto *slot = CONTAINER_OF(work, Slot, mWork);

	Callback callback{ nullptr };
	void *context{ nullptr };

	k_spinlock_key_t key = k_spin_lock(&gLock);
	if (slot->mAcquired) {
		callback = slot->mCallback;
		context = slot->mContext;
	}
	k_spin_unlock(&gLock, key);

	if (callback != nullptr) {
		callback(context);
	}
}

void ExpiryHandler(k_timer *timer)
{
	auto *slot = static_cast<Slot *>(k_timer_user_data_get(timer));

	k_spinlock_key_t key = k_spin_lock(&gLock);
	const bool acquired = slot != nullptr && slot->mAcquired;
	k_spin_unlock(&gLock, key);

	if (acquired && k_work_submit_to_queue(&gWorkQueue, &slot->mWork) < 0) {
		LOG_ERR("Failed to submit timer expiry work");
	}
}

bool IsValid(Handle handle)
{
	return (handle != kInvalidHandle) && (handle >= 0) &&
	       (static_cast<size_t>(handle) < CONFIG_ALIRO_UD_OS_MAX_TIMERS);
}

int StartWorkQueue()
{
	k_work_queue_config config{};
	config.name = "aliro_ud_timer";

	k_work_queue_init(&gWorkQueue);
	k_work_queue_start(&gWorkQueue, gWorkQueueStack, K_THREAD_STACK_SIZEOF(gWorkQueueStack),
			   K_PRIO_PREEMPT(CONFIG_ALIRO_UD_OS_TIMER_THREAD_PRIORITY), &config);
	return 0;
}

SYS_INIT(StartWorkQueue, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);

} // namespace

Handle Acquire(Callback callback, void *context)
{
	if (callback == nullptr) {
		return kInvalidHandle;
	}

	k_spinlock_key_t key = k_spin_lock(&gLock);
	for (size_t i = 0; i < gSlots.size(); i++) {
		auto &slot = gSlots[i];
		if (slot.mAcquired) {
			continue;
		}

		/*
		 * The timer and work item are initialized only once: a released
		 * slot's work item may still be finishing a callback that
		 * released its own timer.
		 */
		if (!slot.mInitialized) {
			k_timer_init(&slot.mTimer, ExpiryHandler, nullptr);
			k_timer_user_data_set(&slot.mTimer, &slot);
			k_work_init(&slot.mWork, WorkHandler);
			slot.mInitialized = true;
		}
		slot.mAcquired = true;
		slot.mCallback = callback;
		slot.mContext = context;
		k_spin_unlock(&gLock, key);
		return static_cast<Handle>(i);
	}
	k_spin_unlock(&gLock, key);

	LOG_ERR("Timer pool exhausted (CONFIG_ALIRO_UD_OS_MAX_TIMERS=%d)", CONFIG_ALIRO_UD_OS_MAX_TIMERS);
	return kInvalidHandle;
}

void Release(Handle handle)
{
	if (!IsValid(handle)) {
		return;
	}

	auto &slot = gSlots[static_cast<size_t>(handle)];

	k_spinlock_key_t key = k_spin_lock(&gLock);
	if (!slot.mAcquired) {
		k_spin_unlock(&gLock, key);
		return;
	}
	slot.mAcquired = false;
	slot.mCallback = nullptr;
	slot.mContext = nullptr;
	k_spin_unlock(&gLock, key);

	k_timer_stop(&slot.mTimer);

	/*
	 * On the timer work queue itself no other timer callback can be
	 * running, and waiting would deadlock a callback that releases its own
	 * timer; cancelling any pending expiry is sufficient there.
	 */
	if (k_current_get() == k_work_queue_thread_get(&gWorkQueue)) {
		(void)k_work_cancel(&slot.mWork);
	} else {
		k_work_sync sync{};
		(void)k_work_cancel_sync(&slot.mWork, &sync);
	}
}

void Start(Handle handle, uint32_t timeoutMs)
{
	if (!IsValid(handle)) {
		return;
	}

	k_timer_start(&gSlots[static_cast<size_t>(handle)].mTimer, K_MSEC(timeoutMs), K_NO_WAIT);
}

void Stop(Handle handle)
{
	if (!IsValid(handle)) {
		return;
	}

	k_timer_stop(&gSlots[static_cast<size_t>(handle)].mTimer);
}

bool IsRunning(Handle handle)
{
	if (!IsValid(handle)) {
		return false;
	}

	return k_timer_remaining_ticks(&gSlots[static_cast<size_t>(handle)].mTimer) != 0;
}

} // namespace Aliro::Interface::UserDevice::Os::Timer
