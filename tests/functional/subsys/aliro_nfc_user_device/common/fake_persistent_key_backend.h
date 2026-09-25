/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#pragma once

/**
 * @brief In-memory fake for `persistent_key_backend.h` (host-test support).
 *
 * Mirrors `persistent_key_backend_psa.cpp`, including its use of
 * `platform/crypto/crypto_internal.h`, but copies `Kpersistent` into a PSA
 * *volatile* key instead of a persistent one: native_sim has no
 * hardware-backed persistent/trusted storage. Only the desired persistent
 * key ID -> underlying volatile PSA key mapping is simulated in RAM,
 * surviving across repeated `AliroUd::PersistentKey::Store::Init()` calls
 * within one test process (simulating reboot). Tests linking this fake must
 * also link `platform/crypto/crypto.cpp`.
 */
namespace AliroUd::PersistentKey::Test {

/** @brief Clears every simulated key mapping, destroying any underlying volatile PSA keys. Call at test start. */
void ResetFakePersistentKeyBackend();

} // namespace AliroUd::PersistentKey::Test
