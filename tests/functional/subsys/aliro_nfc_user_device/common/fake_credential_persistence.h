/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#pragma once

/**
 * @brief In-memory fake for `credential_persistence.h` (APP_PLAN.md AWP3
 * host-test support).
 *
 * Backs every record in RAM instead of Zephyr settings/NVS/ZMS, since
 * native_sim has no hardware-backed flash/trusted storage for the real
 * implementation to use. Records survive across repeated calls to
 * `AliroUd::Credential::Store::Init()` within one test process, which is
 * exactly what a test needs to simulate "reboot": `Store::Init()` re-reads
 * whatever is durably present here, exactly as it would re-read real
 * flash on target.
 *
 * `ArmFault()` supports the AWP3 fault-injection verification requirement:
 * arm exactly one named operation to fail (returning an error without
 * applying the write/erase) on its very next invocation, then let the test
 * observe the resulting rollback/recovery behavior.
 */
namespace AliroUd::Credential::Test {

enum class FaultPoint {
	None,
	SaveSlot,
	EraseSlot,
	SaveJournal,
	EraseJournal,
	SavePreferredTable,
};

/** @brief Clears every record, as if the persistent storage had never been written (first boot). */
void ResetFakePersistence();

/** @brief Arms `point` to fail exactly once on its next invocation; `FaultPoint::None` disarms. */
void ArmFault(FaultPoint point);

/** @brief Whether a fault is currently still armed (not yet consumed). */
bool IsFaultArmed();

} // namespace AliroUd::Credential::Test
