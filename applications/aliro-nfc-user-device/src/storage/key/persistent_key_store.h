/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#pragma once

#include <aliro/errors.h>
#include <aliro/user_device/types.h>

/**
 * @brief `Kpersistent` persistent-key-record engine.
 *
 * Owns the fixed-capacity record table, indexed by the exact
 * `{CredentialHandle, ReaderGroupSubIdentifier}` pair (Aliro 1.0
 * Specification, section 6.2, page 28), and the durable ownership of each
 * record's underlying PSA key material (`persistent_key_backend.h`).
 * `persistent_key.cpp` is a thin adapter translating this API to the
 * `Aliro::Interface::UserDevice::PersistentKey` contract.
 */
namespace AliroUd::PersistentKey::Store {

/**
 * @brief Initializes the persistence backend, loads every persisted record,
 * and recovers any interrupted replacement or deletion.
 *
 * Erases records that cannot be authoritative (malformed, duplicate pair or
 * handle, a key ID outside the slot's pair, or a missing key), then
 * destroys every key at a slot's IDs that its record does not reference.
 * Recovery is idempotent: a second call makes no change. A load failure is
 * returned immediately; any other failure is returned after every remaining
 * recovery step, and a later call retries it.
 */
AliroError Init();

/**
 * @copydoc ::Aliro::Interface::UserDevice::PersistentKey::Lookup
 *
 * Returns `ALIRO_ERROR_UNKNOWN` when no record matches the exact pair, or
 * the backend error when minting the temporary handle fails; both outputs
 * keep their failure values in either case.
 */
AliroError Lookup(::Aliro::UserDevice::CredentialHandle handle,
		   const ::Aliro::UserDevice::ReaderGroupSubIdentifier &readerGroupSubIdentifier,
		   ::Aliro::UserDevice::PersistentKeyHandle &outRecord, ::Aliro::CryptoTypes::KeyId &outKeyId);

/**
 * @copydoc ::Aliro::Interface::UserDevice::PersistentKey::Replace
 *
 * Returns `ALIRO_INVALID_ARGUMENT` for `kInvalidCredentialHandle` or a zero
 * `keyId`, and `ALIRO_NO_MEMORY` when inserting a new pair would exceed the
 * per-credential or global capacity. `keyId` stays owned by the caller on
 * every path. Once the new record is committed the call succeeds, even if
 * the previous key cannot be destroyed; that key is swept later. A record
 * save that reports failure but is found persisted counts as committed.
 */
AliroError Replace(::Aliro::UserDevice::CredentialHandle handle,
		    const ::Aliro::UserDevice::ReaderGroupSubIdentifier &readerGroupSubIdentifier,
		    ::Aliro::CryptoTypes::KeyId keyId, ::Aliro::UserDevice::PersistentKeyHandle &outRecord);

/**
 * @copydoc ::Aliro::Interface::UserDevice::PersistentKey::Delete
 *
 * Deleting an invalid, unknown, or already-deleted handle succeeds without
 * side effects. Erasing the record is the commit point. If the erase fails,
 * the error is returned and the record stays resolvable. If destroying the
 * key then fails, the error is returned, the record is gone, and the key is
 * swept by the next `Init()` or `Replace()` into the same slot.
 */
AliroError Delete(::Aliro::UserDevice::PersistentKeyHandle record);

/**
 * @copydoc ::Aliro::Interface::UserDevice::PersistentKey::Reset
 *
 * Removes every record as `Delete()` does and destroys every key at every
 * slot's IDs. Continues past failures and returns the first one.
 */
AliroError Reset();

/**
 * @brief Deletes every persistent-key record for one credential (used by
 * Credential::Store::Delete/Reset). A credential without records succeeds
 * without side effects. Each record is removed as `Delete()` does; the call
 * continues past failures and returns the first one.
 */
AliroError DeleteAllForCredential(::Aliro::UserDevice::CredentialHandle handle);

} // namespace AliroUd::PersistentKey::Store
