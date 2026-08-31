/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#pragma once

#include <aliro/errors.h>
#include <aliro/types.h>

#include <array>
#include <cstdint>

/**
 * @brief PSA/CRACEN/KMU-backed persistent private-key storage for imported
 * Access Credential private keys (APP_PLAN.md AWP3).
 *
 * The real implementation (`key_backend_psa.cpp`) imports the raw 32-byte
 * big-endian P-256 scalar into persistent PSA key storage at an explicit,
 * caller-chosen key ID (see `credential_types.h` slot ID scheme) with
 * sign-only usage (never `PSA_KEY_USAGE_EXPORT`), so the private key is
 * never exportable once imported; only the derived public key is returned.
 *
 * Host tests link an application-owned fake instead (native_sim has no
 * hardware-backed persistent/trusted storage): see
 * `tests/functional/subsys/aliro_nfc_user_device/credential_store/src/fake_key_backend.cpp`.
 * That fake still performs the scalar-validity check and public-key
 * derivation using real PSA volatile (non-persistent) ECC key import, which
 * has no hardware-trusted-storage dependency, so scalar validation and
 * derived-public-key correctness are exercised with real cryptography; only
 * the *persistence* of the key material is simulated in RAM.
 */
namespace AliroUd::Credential::KeyBackend {

/** @brief Initializes the PSA Crypto subsystem. Idempotent; safe to call multiple times. */
AliroError Init();

/**
 * @brief Validates a raw private-key scalar, imports it at persistent key
 * ID `desiredKeyId`, and derives its public key.
 *
 * @param desiredKeyId The exact persistent PSA key ID to import at (from
 * the deterministic per-slot ID scheme); must not already be in use.
 * @param scalar Exactly 32 big-endian bytes representing a P-256 private
 * key scalar (`Aliro::CryptoTypes::PrivateKey`).
 * @param outPublicKey Set to the derived uncompressed P-256 public key on
 * success.
 *
 * @return `ALIRO_NO_ERROR` on success, `ALIRO_INVALID_ARGUMENT` if `scalar`
 * is not a valid P-256 private-key scalar, `ALIRO_KEY_ALREADY_EXISTS` if
 * `desiredKeyId` is already in use, an error code otherwise. On any
 * failure, no key is left imported at `desiredKeyId`.
 */
AliroError ImportPrivateKeyScalar(::Aliro::CryptoTypes::KeyId desiredKeyId,
				  const std::array<uint8_t, ::Aliro::CryptoTypes::kEccP256KeyPrivateKeyLength> &scalar,
				  ::Aliro::CryptoTypes::PublicKey &outPublicKey);

/**
 * @brief Destroys a previously-imported key.
 *
 * Idempotent: destroying a key ID that is not present is not an error (this
 * matters for boot-time journal recovery, which must tolerate destroying an
 * already-destroyed orphan).
 */
AliroError DestroyKey(::Aliro::CryptoTypes::KeyId keyId);

/** @brief Whether a key is currently present at `keyId`. */
bool IsKeyPresent(::Aliro::CryptoTypes::KeyId keyId);

/**
 * @brief Re-derives the public key for an already-imported key.
 *
 * Used when resuming a transaction at boot where the public key was not
 * itself persisted yet (the committed record only gains the public key once
 * the transaction's metadata write completes).
 */
AliroError GetPublicKey(::Aliro::CryptoTypes::KeyId keyId, ::Aliro::CryptoTypes::PublicKey &outPublicKey);

/**
 * @brief Signs exact data with an already-imported private key, referenced
 * only by its opaque (application-facing) key ID (APP_PLAN.md AWP5,
 * `Aliro::Interface::UserDevice::CredentialSigning::Sign()`).
 *
 * Only this backend translates the application-facing `keyId` to whatever
 * PSA key handle actually holds the key material (the fake host backend
 * maps it to an internal volatile ID; see `key_backend.h`'s class comment);
 * callers outside `storage/credential` must never assume `keyId` is itself
 * a usable PSA key handle.
 *
 * @param keyId The application-facing key identifier (e.g.
 * `PersistedCredential::mKeyId`) previously returned by
 * `ImportPrivateKeyScalar()`.
 * @param data The exact bytes to sign.
 * @param dataLength The length of `data`.
 * @param outSignature The 64-byte `r || s` ECDSA P-256/SHA-256 signature.
 *
 * @return ALIRO_NO_ERROR on success, error code otherwise (including an
 * unknown `keyId`).
 */
AliroError Sign(::Aliro::CryptoTypes::KeyId keyId, const uint8_t *data, size_t dataLength,
		::Aliro::CryptoTypes::Signature &outSignature);

} // namespace AliroUd::Credential::KeyBackend
