/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#pragma once

#include <aliro/errors.h>
#include <aliro/types.h>

/**
 * @brief Aliro profile0000 Reader certificate decompression and validation
 * (APP_PLAN.md AWP5; Aliro 1.0 Specification and Test Plan, 26-42802-001,
 * section 13.2 "Reader certificate requirements" and section 13.3 "Reader
 * certificate compression").
 *
 * `Validate()` performs the full decompression-and-verification pipeline
 * described by section 13.3.2 ("Decompression Steps"): parse the DER
 * profile0000 structure, reconstruct the implicit RFC5280 fields from their
 * fixed default values (13.3, ASN.1 scheme comment block), regenerate the
 * `authorityKeyIdentifier` extension from `issuerPublicKey`, rebuild the
 * exact `TBSCertificate` DER bytes the Reader signed, and verify the
 * ECDSA-P256/SHA-256 signature against `issuerPublicKey`.
 *
 * This is not a thin PSA Crypto binding (APP_PLAN.md AWP5): the DER
 * encoding/decoding here is application-owned. Only the final ECDSA
 * verification and the SHA-1 `keyIdentifier` computation call into PSA
 * Crypto.
 *
 * Per APP_PLAN.md ("Do not add a wall clock and do not enforce Reader
 * certificate validity dates"), `notBefore`/`notAfter` are decoded and
 * validated for shape only; no wall-clock comparison is performed.
 */
namespace AliroUd::Crypto::Certificate {

/** @brief Upper bound on a profile0000-encoded certificate this implementation accepts. */
constexpr size_t kMaxProfile0000Length{ 320 };

/**
 * @brief Decompresses and validates a profile0000 Reader certificate.
 *
 * @param certificate The DER-encoded profile0000 data structure received
 * from the stack (AUTH1 command or LOAD CERT command payload).
 * @param issuerPublicKey The trusted Reader System Issuer CA public key for
 * the exact binding being authenticated; also used to (re)generate the
 * certificate's `authorityKeyIdentifier` extension per the decompression
 * algorithm.
 * @param outSubjectPublicKey The certificate's subject (Reader) public key
 * on success, cleared on failure.
 *
 * @return `ALIRO_NO_ERROR` on success. `ALIRO_INVALID_DATA_FORMAT` if the
 * DER structure is malformed or violates the profile0000 ASN.1 scheme
 * (field sizes/ordering/tags). `ALIRO_INVALID_SIGNATURE` if the
 * reconstructed certificate's signature does not verify against
 * `issuerPublicKey`. `ALIRO_ERROR_INTERNAL` on an internal PSA failure.
 * Per ALIRO-UD-SYRS-P1-024, the error alone never reveals whether
 * `issuerPublicKey` "exists" versus the certificate simply being invalid;
 * callers that must not leak that distinction should treat every non-
 * `ALIRO_NO_ERROR` result identically.
 */
AliroError Validate(::Aliro::ConstData certificate, const ::Aliro::CryptoTypes::PublicKey &issuerPublicKey,
		    ::Aliro::CryptoTypes::PublicKey &outSubjectPublicKey);

} // namespace AliroUd::Crypto::Certificate
