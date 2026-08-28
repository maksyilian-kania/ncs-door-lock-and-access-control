/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#pragma once

#include "credential_types.h"
#include "provisioning.h"

#include <aliro/errors.h>
#include <aliro/user_device/credential.h>

/**
 * @brief Credential and trust persistence engine (APP_PLAN.md AWP3).
 *
 * Owns the fixed-capacity credential slot table, the binding-aware trust
 * model, the preferred-credential table, and the persistent transaction
 * journal spanning `credential_persistence.h` (NVS/ZMS via Zephyr settings)
 * and `key_backend.h` (persistent PSA key storage). This is the only module
 * that talks to both backends directly; `credential.cpp` is a thin adapter
 * translating this API to the `Aliro::Interface::UserDevice::Credential`/
 * `::Trust` contracts, and `src/cli/cli.cpp` drives it for provisioning.
 *
 * Every mutating entry point (`Create()`, `Update()`, `Delete()`, `Reset()`)
 * must only ever be called from within
 * `AliroUd::Lifecycle::RunMutation()` (see `src/lifecycle/lifecycle.h`),
 * which serializes them against each other and against an active NFC
 * session; this module does not itself serialize against the NFC worker
 * thread.
 */
namespace AliroUd::Credential::Store {

/**
 * @brief Initializes the persistence and key backends and performs
 * boot-time transaction-journal recovery.
 *
 * Must be called exactly once at boot, before any other function here and
 * before the NFC transport starts. Finishes or rolls back one interrupted
 * transaction left by a prior journal record (crash/power-loss recovery)
 * and destroys any orphaned staged PSA key.
 */
AliroError Init();

/**
 * @brief Validates a provisioning payload without committing any state
 * change (including validating the private-key scalar, if present, via a
 * transient, immediately-destroyed PSA import).
 */
AliroError Validate(const Provisioning::Payload &input);

/** @brief Validates, then atomically creates a new Access Credential. */
AliroError Create(const Provisioning::Payload &input, ::Aliro::UserDevice::CredentialHandle &outHandle);

/** @brief Validates, then atomically updates an existing Access Credential. */
AliroError Update(::Aliro::UserDevice::CredentialHandle handle, const Provisioning::Payload &input);

/** @brief Deletes an Access Credential and destroys its persistent key. */
AliroError Delete(::Aliro::UserDevice::CredentialHandle handle);

/** @brief Factory-resets every provisioned Access Credential and preferred-credential entry. */
AliroError Reset();

/** @brief Gets non-secret metadata for a credential. */
AliroError GetMetadata(::Aliro::UserDevice::CredentialHandle handle, ::Aliro::UserDevice::CredentialMetadata &outMetadata);

/** @brief Gets the full non-secret persisted record for a credential (for CLI inspection). */
AliroError GetFullRecord(::Aliro::UserDevice::CredentialHandle handle, PersistedCredential &out);

/** @brief Gets the number of reader_group_identifier bindings for a credential. */
AliroError GetGroupBindingCount(::Aliro::UserDevice::CredentialHandle handle, size_t &outCount);

/** @brief Gets one reader_group_identifier binding for a credential. */
AliroError GetGroupBinding(::Aliro::UserDevice::CredentialHandle handle, size_t index,
			   ::Aliro::UserDevice::ReaderGroupIdentifier &outIdentifier);

/**
 * @brief Resolves every Access Credential bound to a reader_group_identifier.
 *
 * When more than one credential matches, the preferred one (if recorded via
 * `SetPreferredCredential()`) is returned first.
 */
AliroError ResolveByReaderGroupIdentifier(const ::Aliro::UserDevice::ReaderGroupIdentifier &readerGroupIdentifier,
					  ::Aliro::UserDevice::CredentialHandle *outHandles, size_t &inOutCount);

/**
 * @brief Gets the directly-bound Reader public key for one exact
 * `{handle, readerGroupIdentifier}` binding.
 */
AliroError GetReaderPublicKey(::Aliro::UserDevice::CredentialHandle handle,
			      const ::Aliro::UserDevice::ReaderGroupIdentifier &readerGroupIdentifier,
			      ::Aliro::CryptoTypes::PublicKey &outPublicKey);

/**
 * @brief Gets the Reader System Issuer CA public key for one exact
 * `{handle, readerGroupIdentifier}` binding.
 */
AliroError GetReaderIssuerPublicKey(::Aliro::UserDevice::CredentialHandle handle,
				    const ::Aliro::UserDevice::ReaderGroupIdentifier &readerGroupIdentifier,
				    ::Aliro::CryptoTypes::PublicKey &outPublicKey);

/**
 * @brief Records which credential is preferred when a reader_group_identifier
 * is bound by more than one (ALIRO-UD-SYRS-P1-010).
 *
 * @param readerGroupIdentifier The shared reader group identifier.
 * @param handle The credential to prefer for this identifier; must already
 * have a matching binding.
 */
AliroError SetPreferredCredential(const ::Aliro::UserDevice::ReaderGroupIdentifier &readerGroupIdentifier,
				  ::Aliro::UserDevice::CredentialHandle handle);

/**
 * @brief Gets the recorded preferred credential for a reader_group_identifier, if any.
 *
 * @return `ALIRO_NO_ERROR` with `outHandle` set if a preference is recorded,
 * `ALIRO_ERROR_UNKNOWN` if none is recorded.
 */
AliroError GetPreferredCredential(const ::Aliro::UserDevice::ReaderGroupIdentifier &readerGroupIdentifier,
				  ::Aliro::UserDevice::CredentialHandle &outHandle);

} // namespace AliroUd::Credential::Store
