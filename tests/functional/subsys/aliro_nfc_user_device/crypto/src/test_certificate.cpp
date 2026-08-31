/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include <zephyr/ztest.h>

#include "platform/crypto/certificate.h"

#include <aliro/types.h>

#include <array>

/*
 * Aliro profile0000 Reader certificate decompression/validation under test
 * (APP_PLAN.md AWP5, platform/crypto/certificate.cpp).
 *
 * "Demo 1" known-answer vector, taken verbatim from the Aliro 1.0
 * Specification and Test Plan, 26-42802-001, section 14.1 "Reader
 * certificate compression examples", page 169 (the "all optional fields
 * default" example): a real profile0000-compressed certificate, its issuer
 * CA public key, and its subject (Reader) public key. Independently
 * re-derived and cross-checked against the spec's own uncompressed X.509
 * form before being hardcoded here: the compressed DER's internal TLV
 * lengths were confirmed to add up exactly (152 bytes total, matching
 * `30 81 95`'s declared length), and `SHA-1(issuerPublicKey)` was confirmed
 * to equal the spec's own printed `authorityKeyIdentifier` value
 * (`2318e55671f08eae212142a817720fb817ee93bf`) and the spec's uncompressed
 * X.509 form's ECDSA signature was independently confirmed to verify
 * against the issuer public key with `cryptography.x509`/`ec.ECDSA` before
 * this vector was trusted for this test.
 */

using namespace Aliro;
using namespace Aliro::CryptoTypes;

namespace {

std::array<uint8_t, 152> Demo1CompressedCertificate()
{
	return { 0x30, 0x81, 0x95, 0x04, 0x02, 0x00, 0x00, 0x30, 0x81, 0x8e, 0x85, 0x42, 0x00, 0x04, 0x84, 0x22, 0x42,
		 0xf6, 0x18, 0x2b, 0xa1, 0xc1, 0x13, 0x8d, 0x32, 0xb7, 0x7f, 0xb9, 0xf7, 0xf3, 0x7b, 0x70, 0x03, 0x4b,
		 0x9f, 0x04, 0x44, 0x3a, 0x5b, 0xea, 0x3c, 0x18, 0x8b, 0xea, 0xdb, 0x36, 0x49, 0x0a, 0x7e, 0x95, 0xf9,
		 0x1a, 0x4c, 0x16, 0x2a, 0xcf, 0xc3, 0x40, 0x1c, 0x3a, 0x4f, 0x4e, 0x5a, 0x59, 0x25, 0x1d, 0x45, 0x24,
		 0x3a, 0xc8, 0x54, 0x4a, 0x66, 0x5c, 0xb9, 0x51, 0x42, 0x2f, 0x86, 0x48, 0x00, 0x30, 0x45, 0x02, 0x21,
		 0x00, 0x87, 0x20, 0xa2, 0xf0, 0x86, 0x26, 0xd5, 0x6b, 0x78, 0x14, 0xb7, 0xe5, 0xbb, 0xe0, 0x43, 0x81,
		 0xe1, 0x83, 0x4c, 0xf9, 0xa2, 0xa5, 0xd4, 0xc8, 0x5c, 0x76, 0x78, 0x36, 0x07, 0xa2, 0x2c, 0xc6, 0x02,
		 0x20, 0x23, 0x6a, 0x4b, 0x75, 0x7c, 0xd4, 0x97, 0xc8, 0x57, 0x0e, 0x84, 0xfa, 0x32, 0x21, 0xbe, 0x99,
		 0xf6, 0xc7, 0x8c, 0xc7, 0xcb, 0xc7, 0x1d, 0x73, 0x28, 0xaa, 0x99, 0xbe, 0x03, 0xf1, 0xec, 0xcf };
}

PublicKey Demo1IssuerPublicKey()
{
	return { 0x04, 0x79, 0x3e, 0x3a, 0x8f, 0x20, 0x42, 0x8d, 0x54, 0xe7, 0x31, 0x80, 0x46, 0xd7, 0x5d,
		 0x05, 0xa8, 0x73, 0x7e, 0xb6, 0xe0, 0x74, 0xe5, 0x14, 0x6a, 0x20, 0x7b, 0xff, 0x62, 0xda,
		 0xe9, 0x0e, 0x24, 0x03, 0x9f, 0x37, 0x28, 0x14, 0xa3, 0x12, 0xc3, 0xcb, 0x82, 0xa5, 0xa9,
		 0x7b, 0xb5, 0xbf, 0xa9, 0xe6, 0x23, 0xa3, 0xcc, 0x88, 0x6b, 0x09, 0xdc, 0x13, 0xd5, 0x3e,
		 0xf0, 0xda, 0x7d, 0xe7, 0xbd };
}

PublicKey Demo1SubjectPublicKey()
{
	return { 0x04, 0x84, 0x22, 0x42, 0xf6, 0x18, 0x2b, 0xa1, 0xc1, 0x13, 0x8d, 0x32, 0xb7, 0x7f, 0xb9,
		 0xf7, 0xf3, 0x7b, 0x70, 0x03, 0x4b, 0x9f, 0x04, 0x44, 0x3a, 0x5b, 0xea, 0x3c, 0x18, 0x8b,
		 0xea, 0xdb, 0x36, 0x49, 0x0a, 0x7e, 0x95, 0xf9, 0x1a, 0x4c, 0x16, 0x2a, 0xcf, 0xc3, 0x40,
		 0x1c, 0x3a, 0x4f, 0x4e, 0x5a, 0x59, 0x25, 0x1d, 0x45, 0x24, 0x3a, 0xc8, 0x54, 0x4a, 0x66,
		 0x5c, 0xb9, 0x51, 0x42, 0x2f };
}

} // namespace

ZTEST_SUITE(aliro_ud_certificate, nullptr, nullptr, nullptr, nullptr, nullptr);

ZTEST(aliro_ud_certificate, test_validate_accepts_spec_demo1_vector_and_extracts_subject_key)
{
	const auto compressed = Demo1CompressedCertificate();
	const auto issuerKey = Demo1IssuerPublicKey();
	const auto expectedSubjectKey = Demo1SubjectPublicKey();

	ConstData certificate{ compressed.data(), compressed.size() };
	PublicKey subjectKey{};
	const AliroError error = AliroUd::Crypto::Certificate::Validate(certificate, issuerKey, subjectKey);

	zassert_equal(ALIRO_NO_ERROR, error);
	zassert_mem_equal(expectedSubjectKey.data(), subjectKey.data(), subjectKey.size());
}

ZTEST(aliro_ud_certificate, test_validate_rejects_wrong_issuer_key)
{
	auto compressed = Demo1CompressedCertificate();
	PublicKey wrongIssuerKey = Demo1IssuerPublicKey();
	wrongIssuerKey[10] ^= 0x01;

	ConstData certificate{ compressed.data(), compressed.size() };
	PublicKey subjectKey{};
	subjectKey.fill(0xAA);
	const AliroError error = AliroUd::Crypto::Certificate::Validate(certificate, wrongIssuerKey, subjectKey);

	zassert_not_equal(ALIRO_NO_ERROR, error);
	for (uint8_t byte : subjectKey) {
		zassert_equal(0, byte, "outSubjectPublicKey must be cleared on failure");
	}
}

ZTEST(aliro_ud_certificate, test_validate_rejects_tampered_signature)
{
	auto compressed = Demo1CompressedCertificate();
	/* Flip a byte inside the DER-encoded signature's `s` component. */
	compressed[compressed.size() - 1] ^= 0x01;
	const auto issuerKey = Demo1IssuerPublicKey();

	ConstData certificate{ compressed.data(), compressed.size() };
	PublicKey subjectKey{};
	const AliroError error = AliroUd::Crypto::Certificate::Validate(certificate, issuerKey, subjectKey);

	zassert_equal(ALIRO_INVALID_SIGNATURE, error);
}

ZTEST(aliro_ud_certificate, test_validate_rejects_truncated_certificate)
{
	const auto compressed = Demo1CompressedCertificate();
	ConstData certificate{ compressed.data(), compressed.size() - 1 };
	PublicKey subjectKey{};
	const AliroError error = AliroUd::Crypto::Certificate::Validate(certificate, Demo1IssuerPublicKey(), subjectKey);

	zassert_equal(ALIRO_INVALID_DATA_FORMAT, error);
}

ZTEST(aliro_ud_certificate, test_validate_rejects_null_certificate)
{
	PublicKey subjectKey{};
	const AliroError error =
		AliroUd::Crypto::Certificate::Validate(ConstData{ nullptr, 0 }, Demo1IssuerPublicKey(), subjectKey);

	zassert_equal(ALIRO_INVALID_DATA_FORMAT, error);
}
