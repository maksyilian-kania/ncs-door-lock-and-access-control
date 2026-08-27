/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <aliro/user_device/interface.h>

LOG_MODULE_REGISTER(aliro_ud_os, CONFIG_ALIRO_UD_OS_LOG_LEVEL);

namespace {

/*
 * Zephyr k_mutex is already reentrant for its owning thread, matching every
 * current call site (Aliro::UserDevice::StackMutexGuard), which locks/unlocks
 * from a single thread at a time (the NFC/stack worker thread, AWP1).
 */
K_MUTEX_DEFINE(sStackMutex);

} // namespace

namespace Aliro::Interface::UserDevice::Os::Mutex {

void Lock()
{
	k_mutex_lock(&sStackMutex, K_FOREVER);
}

void Unlock()
{
	k_mutex_unlock(&sStackMutex);
}

} // namespace Aliro::Interface::UserDevice::Os::Mutex
