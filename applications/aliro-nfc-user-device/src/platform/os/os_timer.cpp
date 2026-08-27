/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <aliro/user_device/interface.h>

#include <array>

LOG_MODULE_DECLARE(aliro_ud_os, CONFIG_ALIRO_UD_OS_LOG_LEVEL);

namespace Aliro::Interface::UserDevice::Os::Timer {
namespace {

struct Slot {
	bool mAcquired{ false };
	Callback mCallback{ nullptr };
	void *mContext{ nullptr };
	k_timer mTimer{};
};

/*
 * k_timer's expiry_fn runs in the system clock ISR context, so it must not
 * block. It only forwards to the stack-supplied Callback, which (for every
 * current caller, Aliro::UserDevice::UserDeviceSession::WatchdogExpiredCallback)
 * itself only defers further work through Os::QueueEvent() - safe from ISR
 * context.
 */
void ExpiryHandler(k_timer *timer)
{
	auto *slot = static_cast<Slot *>(k_timer_user_data_get(timer));
	if (slot != nullptr && slot->mAcquired && (slot->mCallback != nullptr)) {
		slot->mCallback(slot->mContext);
	}
}

std::array<Slot, CONFIG_ALIRO_UD_OS_MAX_TIMERS> gSlots{};

bool IsValid(Handle handle)
{
	return (handle != kInvalidHandle) && (handle >= 0) &&
	       (static_cast<size_t>(handle) < CONFIG_ALIRO_UD_OS_MAX_TIMERS);
}

} // namespace

Handle Acquire(Callback callback, void *context)
{
	for (size_t i = 0; i < gSlots.size(); i++) {
		if (!gSlots[i].mAcquired) {
			gSlots[i].mAcquired = true;
			gSlots[i].mCallback = callback;
			gSlots[i].mContext = context;
			k_timer_init(&gSlots[i].mTimer, ExpiryHandler, nullptr);
			k_timer_user_data_set(&gSlots[i].mTimer, &gSlots[i]);
			return static_cast<Handle>(i);
		}
	}

	LOG_ERR("Timer pool exhausted (CONFIG_ALIRO_UD_OS_MAX_TIMERS=%d)", CONFIG_ALIRO_UD_OS_MAX_TIMERS);
	return kInvalidHandle;
}

void Release(Handle handle)
{
	if (!IsValid(handle)) {
		return;
	}

	auto &slot = gSlots[static_cast<size_t>(handle)];
	k_timer_stop(&slot.mTimer);
	slot = Slot{};
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
