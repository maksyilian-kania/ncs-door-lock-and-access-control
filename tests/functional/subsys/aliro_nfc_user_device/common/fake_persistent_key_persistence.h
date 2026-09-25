/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#pragma once

/**
 * @brief In-memory fake for `persistent_key_persistence.h` (host-test
 * support).
 *
 * Mirrors `fake_mailbox_persistence.h`: backs every record slot in RAM
 * instead of Zephyr settings/NVS/ZMS, surviving across repeated calls to
 * `AliroUd::PersistentKey::Store::Init()` within one test process
 * (simulating reboot).
 */
namespace AliroUd::PersistentKey::Test {

/** @brief Clears every record, as if the persistent storage had never been written (first boot). */
void ResetFakePersistentKeyPersistence();

} // namespace AliroUd::PersistentKey::Test
