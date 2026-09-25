/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include <zephyr/kernel.h>
#include <zephyr/ztest.h>

#include <aliro/user_device/interface.h>

#include <array>
#include <atomic>

/*
 * The application's Aliro::Interface::UserDevice::Os::Timer adapter against
 * its public contract (include/aliro/user_device/interface.h): callbacks run
 * in thread context, because stack callbacks such as
 * UserDeviceSession::WatchdogExpiredCallback() take the stack mutex, and
 * Release() is synchronously quiescent.
 */

namespace Timer = Aliro::Interface::UserDevice::Os::Timer;

namespace {

constexpr uint32_t kShortTimeoutMs{ 10 };

struct Probe {
	std::atomic<int> mCalls{ 0 };
	std::atomic<bool> mInIsr{ false };
	std::atomic<bool> mCompleted{ false };
	k_tid_t mThread{ nullptr };
	Timer::Handle mHandle{ Timer::kInvalidHandle };
	bool mBlock{ false };
	bool mReleaseSelf{ false };
	k_sem mEntered{};
	k_sem mProceed{};
};

K_MUTEX_DEFINE(sCallbackMutex);

void ProbeCallback(void *context)
{
	auto *probe = static_cast<Probe *>(context);

	probe->mInIsr = k_is_in_isr();
	probe->mThread = k_current_get();

	/* The stack watchdog callback takes the stack mutex; so does this one. */
	k_mutex_lock(&sCallbackMutex, K_FOREVER);
	k_mutex_unlock(&sCallbackMutex);

	if (probe->mBlock) {
		k_sem_give(&probe->mEntered);
		k_sem_take(&probe->mProceed, K_FOREVER);
	}
	if (probe->mReleaseSelf) {
		Timer::Release(probe->mHandle);
	}

	probe->mCalls++;
	probe->mCompleted = true;
}

void InitProbe(Probe &probe)
{
	k_sem_init(&probe.mEntered, 0, 1);
	k_sem_init(&probe.mProceed, 0, 1);
}

Probe *sReleaseTarget{ nullptr };
std::atomic<bool> sReleaseReturned{ false };
std::atomic<bool> sCallbackCompletedAtRelease{ false };

void ReleaseThreadEntry(void *, void *, void *)
{
	Timer::Release(sReleaseTarget->mHandle);
	sCallbackCompletedAtRelease = sReleaseTarget->mCompleted.load();
	sReleaseReturned = true;
}

K_THREAD_STACK_DEFINE(sReleaseThreadStack, 2048);
k_thread sReleaseThread{};

} // namespace

ZTEST_SUITE(aliro_ud_os_timer, nullptr, nullptr, nullptr, nullptr, nullptr);

/** @brief An expiry invokes the callback exactly once, on a thread (not the timer ISR), where it may block on a mutex. */
ZTEST(aliro_ud_os_timer, test_callback_runs_once_in_thread_context)
{
	Probe probe{};
	InitProbe(probe);
	probe.mHandle = Timer::Acquire(ProbeCallback, &probe);
	zassert_not_equal(Timer::kInvalidHandle, probe.mHandle);

	Timer::Start(probe.mHandle, kShortTimeoutMs);
	k_msleep(5 * kShortTimeoutMs);

	zassert_equal(1, probe.mCalls.load(), "A one-shot timer must fire exactly once");
	zassert_false(probe.mInIsr.load(), "The callback must not run in ISR context");
	zassert_not_equal(k_current_get(), probe.mThread, "The callback must run on the timer work queue");
	zassert_false(Timer::IsRunning(probe.mHandle));

	Timer::Release(probe.mHandle);
}

/** @brief Release() does not return while a callback for the same timer is still executing. */
ZTEST(aliro_ud_os_timer, test_release_waits_for_running_callback)
{
	Probe probe{};
	InitProbe(probe);
	probe.mBlock = true;
	probe.mHandle = Timer::Acquire(ProbeCallback, &probe);
	zassert_not_equal(Timer::kInvalidHandle, probe.mHandle);

	Timer::Start(probe.mHandle, kShortTimeoutMs);
	zassert_equal(0, k_sem_take(&probe.mEntered, K_MSEC(500)), "The callback must start");

	sReleaseTarget = &probe;
	sReleaseReturned = false;
	sCallbackCompletedAtRelease = false;
	k_thread_create(&sReleaseThread, sReleaseThreadStack, K_THREAD_STACK_SIZEOF(sReleaseThreadStack),
			ReleaseThreadEntry, nullptr, nullptr, nullptr, K_PRIO_PREEMPT(1), 0, K_NO_WAIT);

	k_msleep(50);
	zassert_false(sReleaseReturned.load(), "Release() must wait for the executing callback");

	k_sem_give(&probe.mProceed);
	zassert_equal(0, k_thread_join(&sReleaseThread, K_MSEC(500)));
	zassert_true(sReleaseReturned.load());
	zassert_true(sCallbackCompletedAtRelease.load(), "The callback must have completed before Release() returned");
	zassert_equal(1, probe.mCalls.load());
}

/** @brief Releasing a started timer before it expires cancels the callback. */
ZTEST(aliro_ud_os_timer, test_release_cancels_pending_expiry)
{
	Probe probe{};
	InitProbe(probe);
	probe.mHandle = Timer::Acquire(ProbeCallback, &probe);
	zassert_not_equal(Timer::kInvalidHandle, probe.mHandle);

	Timer::Start(probe.mHandle, 2 * kShortTimeoutMs);
	Timer::Release(probe.mHandle);
	k_msleep(10 * kShortTimeoutMs);

	zassert_equal(0, probe.mCalls.load(), "No callback may run after Release()");
}

/** @brief A callback may release its own timer without deadlocking, and the slot is then reusable. */
ZTEST(aliro_ud_os_timer, test_callback_may_release_its_own_timer)
{
	Probe probe{};
	InitProbe(probe);
	probe.mReleaseSelf = true;
	probe.mHandle = Timer::Acquire(ProbeCallback, &probe);
	zassert_not_equal(Timer::kInvalidHandle, probe.mHandle);

	Timer::Start(probe.mHandle, kShortTimeoutMs);
	k_msleep(5 * kShortTimeoutMs);
	zassert_equal(1, probe.mCalls.load());

	Probe next{};
	InitProbe(next);
	next.mHandle = Timer::Acquire(ProbeCallback, &next);
	zassert_equal(probe.mHandle, next.mHandle, "The self-released slot must be reusable");
	Timer::Start(next.mHandle, kShortTimeoutMs);
	k_msleep(5 * kShortTimeoutMs);
	zassert_equal(1, next.mCalls.load());
	zassert_equal(1, probe.mCalls.load(), "The released callback/context pair must never run again");
	Timer::Release(next.mHandle);
}

/** @brief Stop() cancels a running timer; IsRunning() reflects both states; Start() restarts. */
ZTEST(aliro_ud_os_timer, test_stop_and_restart)
{
	Probe probe{};
	InitProbe(probe);
	probe.mHandle = Timer::Acquire(ProbeCallback, &probe);
	zassert_not_equal(Timer::kInvalidHandle, probe.mHandle);

	Timer::Start(probe.mHandle, 2 * kShortTimeoutMs);
	zassert_true(Timer::IsRunning(probe.mHandle));
	Timer::Stop(probe.mHandle);
	zassert_false(Timer::IsRunning(probe.mHandle));
	k_msleep(10 * kShortTimeoutMs);
	zassert_equal(0, probe.mCalls.load(), "A stopped timer must not fire");

	Timer::Start(probe.mHandle, kShortTimeoutMs);
	k_msleep(5 * kShortTimeoutMs);
	zassert_equal(1, probe.mCalls.load(), "A restarted timer must fire");

	Timer::Release(probe.mHandle);
}

/** @brief The pool holds exactly CONFIG_ALIRO_UD_OS_MAX_TIMERS slots; invalid handles and callbacks are rejected. */
ZTEST(aliro_ud_os_timer, test_pool_bounds_and_invalid_arguments)
{
	std::array<Probe, CONFIG_ALIRO_UD_OS_MAX_TIMERS> probes{};
	for (auto &probe : probes) {
		InitProbe(probe);
		probe.mHandle = Timer::Acquire(ProbeCallback, &probe);
		zassert_not_equal(Timer::kInvalidHandle, probe.mHandle);
	}

	Probe extra{};
	zassert_equal(Timer::kInvalidHandle, Timer::Acquire(ProbeCallback, &extra), "The pool must be exhausted");
	for (auto &probe : probes) {
		Timer::Release(probe.mHandle);
	}

	zassert_equal(Timer::kInvalidHandle, Timer::Acquire(nullptr, &extra), "A null callback must be rejected");
	Timer::Release(Timer::kInvalidHandle);
	Timer::Start(CONFIG_ALIRO_UD_OS_MAX_TIMERS, kShortTimeoutMs);
	zassert_false(Timer::IsRunning(Timer::kInvalidHandle));
}
