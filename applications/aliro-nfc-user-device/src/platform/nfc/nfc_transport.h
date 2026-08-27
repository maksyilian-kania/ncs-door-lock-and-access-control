/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#pragma once

namespace AliroUd::Nfc {

/**
 * @brief Starts the NFC/stack worker thread and the NFC-A Type 4 Tag
 * (ISO-DEP) listen-mode transport.
 *
 * Must be called once at boot, after `Aliro::UserDeviceStack::Instance().Init()`.
 *
 * @return 0 on success, a negative errno value otherwise.
 */
int Start();

} // namespace AliroUd::Nfc
