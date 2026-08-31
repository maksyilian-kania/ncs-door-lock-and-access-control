/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#pragma once

/**
 * @brief In-memory fake for `mailbox_persistence.h` (APP_PLAN.md AWP6
 * host-test support).
 *
 * Mirrors `fake_credential_persistence.h`: backs every record in RAM
 * instead of Zephyr settings/NVS/ZMS, survives across repeated calls to
 * `AliroUd::Mailbox::Store::Init()` within one test process (simulating
 * reboot), and supports arming exactly one named operation to fail once,
 * for the "recovery from reset/power-loss injection at each mailbox commit
 * transition" verification requirement (APP_PLAN.md AWP6).
 */
namespace AliroUd::Mailbox::Test {

enum class FaultPoint {
	None,
	SaveSlot,
	EraseSlot,
};

/** @brief Clears every record, as if the persistent storage had never been written (first boot). */
void ResetFakeMailboxPersistence();

/** @brief Arms `point` to fail exactly once on its next invocation; `FaultPoint::None` disarms. */
void ArmMailboxFault(FaultPoint point);

} // namespace AliroUd::Mailbox::Test
