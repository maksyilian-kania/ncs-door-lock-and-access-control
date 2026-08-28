/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#pragma once

#include "credential_types.h"

#include <aliro/errors.h>

/**
 * @brief Non-secret persistent storage for the credential/trust data model
 * (APP_PLAN.md AWP3).
 *
 * This is a small, purpose-built abstraction (not a generic key-value
 * store) so that:
 *  - the real implementation (`credential_persistence_settings.cpp`) can be
 *    a thin wrapper over Zephyr settings/NVS-or-ZMS, and
 *  - host tests can link an application-owned in-memory fake
 *    (`fake_credential_persistence.cpp`, in the test tree) that supports
 *    deterministic fault injection before/after every write, per
 *    APP_PLAN.md AWP3's verification requirements.
 *
 * Every function is synchronous and blocking; callers serialize concurrent
 * access themselves (the credential store's own mutex).
 */
namespace AliroUd::Credential::Persistence {

/** @brief Initializes the persistence backend (loads the settings subsystem on target). */
AliroError Init();

/**
 * @brief Loads one credential slot.
 *
 * @param slotIndex Zero-based slot index, less than `kMaxCredentials`.
 * @param out Filled with the stored record if present.
 * @param outPresent Set to true if a record exists for this slot, false otherwise.
 */
AliroError LoadSlot(size_t slotIndex, PersistedCredential &out, bool &outPresent);

/** @brief Persists one credential slot, overwriting any previous record. */
AliroError SaveSlot(size_t slotIndex, const PersistedCredential &value);

/** @brief Erases one credential slot's record, if any. Idempotent. */
AliroError EraseSlot(size_t slotIndex);

/** @brief Loads the transaction journal record. */
AliroError LoadJournal(JournalRecord &out, bool &outPresent);

/** @brief Persists the transaction journal record, overwriting any previous one. */
AliroError SaveJournal(const JournalRecord &value);

/** @brief Erases the transaction journal record, if any. Idempotent. */
AliroError EraseJournal();

/** @brief Loads the preferred-credential table. Absent-on-disk is reported as an all-empty table. */
AliroError LoadPreferredTable(PreferredTable &out);

/** @brief Persists the preferred-credential table, overwriting any previous one. */
AliroError SavePreferredTable(const PreferredTable &value);

} // namespace AliroUd::Credential::Persistence
