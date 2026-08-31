/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include "authorization_window.h"

namespace AliroUd::Authorization {

void Window::Open(int64_t nowMs, uint32_t durationMs)
{
	const k_spinlock_key_t key = k_spin_lock(&mLock);
	mHasDeadline = true;
	mDeadlineMs = nowMs + static_cast<int64_t>(durationMs);
	k_spin_unlock(&mLock, key);
}

void Window::Close()
{
	const k_spinlock_key_t key = k_spin_lock(&mLock);
	mHasDeadline = false;
	mDeadlineMs = 0;
	k_spin_unlock(&mLock, key);
}

bool Window::IsValid(int64_t nowMs) const
{
	const k_spinlock_key_t key = k_spin_lock(&mLock);
	const bool valid = mHasDeadline && nowMs < mDeadlineMs;
	k_spin_unlock(&mLock, key);
	return valid;
}

::Aliro::UserDevice::AuthorizationState Window::GetState(int64_t nowMs) const
{
	return IsValid(nowMs) ? ::Aliro::UserDevice::AuthorizationState::Authorized
			      : ::Aliro::UserDevice::AuthorizationState::Required;
}

int64_t Window::GetRemainingMs(int64_t nowMs) const
{
	const k_spinlock_key_t key = k_spin_lock(&mLock);
	const int64_t remaining = mHasDeadline ? (mDeadlineMs - nowMs) : 0;
	k_spin_unlock(&mLock, key);
	return remaining > 0 ? remaining : 0;
}

Window &GlobalWindow()
{
	static Window window{};
	return window;
}

} // namespace AliroUd::Authorization
