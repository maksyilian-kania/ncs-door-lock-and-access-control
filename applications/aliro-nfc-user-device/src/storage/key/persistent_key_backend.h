/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#pragma once

#include <aliro/errors.h>
#include <aliro/types.h>

/**
 * @brief PSA-backed durable ownership of `Kpersistent` key material.
 *
 * `Aliro::Interface::UserDevice::PersistentKey::Replace()` receives a
 * `CryptoTypes::KeyId` that its caller (the `ncs-aliro` stack) destroys
 * immediately after `Replace()` returns, regardless of success or failure.
 * This backend durably copies the referenced key's material, via
 * `psa_copy_key()`, into a persistent PSA key slot this module owns *before*
 * `Replace()` returns, and later mints a fresh, throwaway volatile copy for
 * `Lookup()` to hand back (so the caller's own later `DestroyKey()` on that
 * copy never touches the durable original).
 *
 * The real implementation (`persistent_key_backend_psa.cpp`) is disabled
 * under `ZTEST`; host tests link an application-owned in-memory fake
 * instead (native_sim has no hardware-backed persistent storage).
 */
namespace AliroUd::PersistentKey::Backend {

/**
 * @brief Durably copies the derive-capable key material referenced by
 * `sourceKeyId` into a new persistent PSA key at `desiredPersistentKeyId`.
 *
 * @param sourceKeyId The caller's transient `KeyId` (as passed to
 * `PersistentKey::Replace()`); left untouched by this call.
 * @param desiredPersistentKeyId The exact persistent PSA key id to copy
 * into; must not already be in use.
 * @param outActualPersistentKeyId Set to `desiredPersistentKeyId` on
 * success.
 *
 * @return `ALIRO_NO_ERROR` on success, error code otherwise. On failure, no
 * persistent key is left at `desiredPersistentKeyId`.
 */
AliroError DurablyOwn(::Aliro::CryptoTypes::KeyId sourceKeyId, ::Aliro::CryptoTypes::KeyId desiredPersistentKeyId,
		      ::Aliro::CryptoTypes::KeyId &outActualPersistentKeyId);

/**
 * @brief Mints a fresh, throwaway volatile `KeyId` wrapping a copy of the
 * durable key material at `persistedKeyId`.
 *
 * The caller is expected to eventually destroy the returned `KeyId` via
 * `Aliro::Interface::UserDevice::Crypto::DestroyKey()`; doing so never
 * affects `persistedKeyId`.
 */
AliroError MintVolatileHandle(::Aliro::CryptoTypes::KeyId persistedKeyId,
			      ::Aliro::CryptoTypes::KeyId &outVolatileKeyId);

/**
 * @brief Reports whether a durable persistent key exists at `persistedKeyId`.
 *
 * @param persistedKeyId The persistent PSA key id to query.
 * @param outExists Set to true only when the key is known to exist.
 *
 * @return `ALIRO_NO_ERROR` when presence was determined, error code when
 * the key store could not answer (`outExists` is then false and must not be
 * taken as "absent").
 */
AliroError Exists(::Aliro::CryptoTypes::KeyId persistedKeyId, bool &outExists);

/**
 * @brief Destroys a durable persistent key previously created by `DurablyOwn()`.
 *
 * Idempotent: an absent key succeeds. Returns an error, leaving the key in
 * place, when presence cannot be determined or destruction fails.
 */
AliroError Destroy(::Aliro::CryptoTypes::KeyId persistedKeyId);

} // namespace AliroUd::PersistentKey::Backend
