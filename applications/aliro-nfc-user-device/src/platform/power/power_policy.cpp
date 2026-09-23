/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include "power_policy.h"

namespace AliroUd::Power {

void Policy::NotifyFieldOn()
{
	const k_spinlock_key_t key = k_spin_lock(&mLock);
	mIdleDeadlineSet = false;
	k_spin_unlock(&mLock, key);
}

void Policy::NotifyFieldOff(int64_t nowMs, uint32_t idleDelayMs)
{
	const k_spinlock_key_t key = k_spin_lock(&mLock);
	if (!mHoldAwake) {
		mIdleDeadlineSet = true;
		mIdleDeadlineMs = nowMs + static_cast<int64_t>(idleDelayMs);
	}
	k_spin_unlock(&mLock, key);
}

void Policy::ArmAutoSleep(int64_t nowMs, uint32_t delayMs)
{
	const k_spinlock_key_t key = k_spin_lock(&mLock);
	mIdleDeadlineSet = true;
	mIdleDeadlineMs = nowMs + static_cast<int64_t>(delayMs);
	k_spin_unlock(&mLock, key);
}

void Policy::RequestManualSleep()
{
	const k_spinlock_key_t key = k_spin_lock(&mLock);
	mManualSleepRequested = true;
	k_spin_unlock(&mLock, key);
}

void Policy::SetHoldAwake()
{
	const k_spinlock_key_t key = k_spin_lock(&mLock);
	mHoldAwake = true;
	/* Cancel any auto idle-timeout that was armed at boot: hold-awake is indefinite. */
	mIdleDeadlineSet = false;
	k_spin_unlock(&mLock, key);
}

void Policy::ClearHoldAwake()
{
	const k_spinlock_key_t key = k_spin_lock(&mLock);
	mHoldAwake = false;
	k_spin_unlock(&mLock, key);
}

bool Policy::IsHoldAwake() const
{
	const k_spinlock_key_t key = k_spin_lock(&mLock);
	const bool held = mHoldAwake;
	k_spin_unlock(&mLock, key);
	return held;
}

void Policy::BeginMutationGuard()
{
	const k_spinlock_key_t key = k_spin_lock(&mLock);
	mMutationGuardDepth++;
	k_spin_unlock(&mLock, key);
}

void Policy::EndMutationGuard()
{
	const k_spinlock_key_t key = k_spin_lock(&mLock);
	if (mMutationGuardDepth > 0) {
		mMutationGuardDepth--;
	}
	k_spin_unlock(&mLock, key);
}

bool Policy::ShouldPowerOff(int64_t nowMs, bool windowValid)
{
	const k_spinlock_key_t key = k_spin_lock(&mLock);

	if (mMutationGuardDepth > 0) {
		k_spin_unlock(&mLock, key);
		return false;
	}

	/*
	 * Manual (Button 1) sleep always wins and is never gated by the
	 * authorization window; it also supersedes any still-armed auto-sleep
	 * deadline (one power-off is enough).
	 */
	if (mManualSleepRequested) {
		mManualSleepRequested = false;
		mIdleDeadlineSet = false;
		k_spin_unlock(&mLock, key);
		return true;
	}

	/* Hold-awake suppresses only the automatic idle-timeout path, never the manual exit press above. */
	if (mHoldAwake) {
		k_spin_unlock(&mLock, key);
		return false;
	}

	if (mIdleDeadlineSet && !windowValid && (nowMs >= mIdleDeadlineMs)) {
		mIdleDeadlineSet = false;
		k_spin_unlock(&mLock, key);
		return true;
	}

	k_spin_unlock(&mLock, key);
	return false;
}

bool Policy::IsMutationGuardActive() const
{
	const k_spinlock_key_t key = k_spin_lock(&mLock);
	const bool active = mMutationGuardDepth > 0;
	k_spin_unlock(&mLock, key);
	return active;
}

bool Policy::IsShutdownPending() const
{
	const k_spinlock_key_t key = k_spin_lock(&mLock);
	const bool pending = mManualSleepRequested || mIdleDeadlineSet;
	k_spin_unlock(&mLock, key);
	return pending;
}

} // namespace AliroUd::Power
