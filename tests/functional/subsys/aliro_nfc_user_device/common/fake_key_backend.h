/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#pragma once

/**
 * @brief In-memory fake for `key_backend.h` (APP_PLAN.md AWP3 host-test
 * support).
 *
 * Real scalar validation and public-key derivation, via a real PSA
 * *volatile* (non-persistent) ECC key pair import: native_sim has no
 * hardware-backed persistent/trusted storage, but volatile PSA crypto
 * needs none, so this exercises genuine P-256 scalar-validity checking
 * and public-key math rather than faking the cryptography itself. Only
 * the *persistence* of the app-level desired key ID -> underlying PSA
 * key mapping is simulated in RAM, surviving across a simulated "reboot"
 * (repeated `AliroUd::Credential::Store::Init()` calls within one test
 * process) exactly like the real persistent-key backend would.
 */
namespace AliroUd::Credential::Test {

/** @brief Clears every simulated key mapping, destroying any underlying volatile PSA keys. Call at test start. */
void ResetFakeKeyBackend();

} // namespace AliroUd::Credential::Test
