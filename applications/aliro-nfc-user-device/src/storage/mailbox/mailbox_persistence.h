/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#pragma once

#include "mailbox_types.h"

#include <aliro/errors.h>

/**
 * @brief Persistent storage for per-credential mailbox byte content
 * (APP_PLAN.md AWP6).
 *
 * Mirrors `storage/credential/credential_persistence.h`'s split: the real
 * implementation (`mailbox_persistence_settings.cpp`) is a thin wrapper
 * over Zephyr settings/NVS-or-ZMS, and host tests link an application-owned
 * in-memory fake (`fake_mailbox_persistence.cpp`, in the test tree)
 * supporting deterministic fault injection before/after every write, per
 * APP_PLAN.md AWP6's "recovery from reset/power-loss injection at each
 * mailbox commit transition" verification requirement.
 *
 * Every function is synchronous and blocking; callers serialize concurrent
 * access themselves (the mailbox store's own mutex).
 */
namespace AliroUd::Mailbox::Persistence {

/** @brief Initializes the persistence backend (loads the settings subsystem on target). */
AliroError Init();

/**
 * @brief Loads one mailbox slot.
 *
 * @param slotIndex Zero-based slot index, less than `kMaxCredentials`.
 * @param out Filled with the stored record if present.
 * @param outPresent Set to true if a record exists for this slot, false otherwise.
 */
AliroError LoadSlot(size_t slotIndex, MailboxRecord &out, bool &outPresent);

/** @brief Persists one mailbox slot, overwriting any previous record. */
AliroError SaveSlot(size_t slotIndex, const MailboxRecord &value);

/** @brief Erases one mailbox slot's record, if any. Idempotent. */
AliroError EraseSlot(size_t slotIndex);

} // namespace AliroUd::Mailbox::Persistence
