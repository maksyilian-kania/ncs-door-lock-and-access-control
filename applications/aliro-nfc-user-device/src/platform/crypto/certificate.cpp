/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include "certificate.h"

#include <psa/crypto.h>
#include <zephyr/logging/log.h>

#include <array>
#include <cstring>

LOG_MODULE_DECLARE(aliro_ud_crypto, CONFIG_ALIRO_UD_CRYPTO_LOG_LEVEL);

/*
 * profile0000 DER structure (Aliro 1.0 Specification and Test Plan,
 * 26-42802-001, section 13.3):
 *
 *   Profile0000 ::= SEQUENCE { profile OCTET STRING(2), data Profile0000Data }
 *   Profile0000Data ::= SEQUENCE {
 *     serialNumber [0] OCTET STRING (1..20) OPTIONAL,
 *     issuer       [1] OCTET STRING (1..32) OPTIONAL,
 *     notBefore    [2] OCTET STRING (13..15) OPTIONAL,
 *     notAfter     [3] OCTET STRING (13..15) OPTIONAL,
 *     subject      [4] OCTET STRING (1..32) OPTIONAL,
 *     publicKey    [5] OCTET STRING,
 *     signature    [6] OCTET STRING,
 *   }
 *
 * The reference/default RFC5280 TBSCertificate this reconstructs from those
 * fields, and its default field values, are taken verbatim from the exact
 * bytes of the Aliro 1.0 Specification's own worked examples (section 14.1,
 * "Reader certificate compression examples", pages 170-173): default
 * serialNumber = 0x01, default issuer/subject commonName = "issuer"/
 * "subject" (UTF8String), default notBefore/notAfter =
 * "200101000000Z"/"490101000000Z" (UTCTime). The authorityKeyIdentifier
 * extension's keyIdentifier is SHA-1(issuerPublicKey), matching the key_slot
 * computation method described in section 8.3.3.4.2 (RFC5280 method 1: the
 * 160-bit SHA-1 hash of the subjectPublicKey BIT STRING content, excluding
 * tag/length/unused-bits octet).
 */
namespace AliroUd::Crypto::Certificate {
namespace {

using ::Aliro::ConstData;
using ::Aliro::CryptoTypes::PublicKey;
using ::Aliro::CryptoTypes::Signature;

constexpr size_t kMaxTbsBufferLength{ 400 };
constexpr size_t kAkiLength{ 20 };
constexpr size_t kEccP256PublicKeyLength{ ::Aliro::CryptoTypes::kEccP256PublicKeyLength };

/** @brief Minimal sequential DER TLV reader for the fixed profile0000/X.509 shapes used here. */
class DerReader {
public:
	DerReader(const uint8_t *data, size_t len) : mData(data), mLen(len) {}

	bool PeekTag(uint8_t &tag) const
	{
		if (mPos >= mLen) {
			return false;
		}
		tag = mData[mPos];
		return true;
	}

	bool ReadTagLenValue(uint8_t expectedTag, const uint8_t *&outValue, size_t &outLen)
	{
		if (mPos >= mLen || mData[mPos] != expectedTag) {
			return false;
		}
		mPos++;

		size_t contentLen{};
		if (!ReadLength(contentLen)) {
			return false;
		}
		if (contentLen > mLen - mPos) {
			return false;
		}

		outValue = mData + mPos;
		outLen = contentLen;
		mPos += contentLen;
		return true;
	}

	bool AtEnd() const { return mPos == mLen; }

private:
	bool ReadLength(size_t &outLen)
	{
		if (mPos >= mLen) {
			return false;
		}
		const uint8_t first = mData[mPos++];
		if ((first & 0x80) == 0) {
			outLen = first;
			return true;
		}

		const uint8_t numBytes = first & 0x7f;
		if (numBytes == 0 || numBytes > 2 || numBytes > mLen - mPos) {
			return false;
		}

		size_t value{};
		for (uint8_t i = 0; i < numBytes; i++) {
			value = (value << 8) | mData[mPos++];
		}
		outLen = value;
		return true;
	}

	const uint8_t *mData;
	size_t mLen;
	size_t mPos{ 0 };
};

/**
 * @brief Minimal DER TLV writer building content from the end of a fixed
 * buffer backward ("prepend"), so nested SEQUENCE/context-tag wrapping never
 * needs a dynamic/heap-allocated tree: each `WrapTLV()` call encloses
 * exactly the bytes written since its matching earlier cursor position.
 */
class DerWriter {
public:
	DerWriter(uint8_t *buffer, size_t capacity) : mBuffer(buffer), mCapacity(capacity), mCursor(capacity) {}

	bool PrependRaw(const uint8_t *data, size_t len)
	{
		if (len > mCursor) {
			return false;
		}
		mCursor -= len;
		memcpy(mBuffer + mCursor, data, len);
		return true;
	}

	bool PrependLength(size_t len)
	{
		if (len < 0x80) {
			const uint8_t b = static_cast<uint8_t>(len);
			return PrependRaw(&b, 1);
		} else if (len <= 0xff) {
			const uint8_t b[2] = { 0x81, static_cast<uint8_t>(len) };
			return PrependRaw(b, sizeof(b));
		} else if (len <= 0xffff) {
			const uint8_t b[3] = { 0x82, static_cast<uint8_t>(len >> 8), static_cast<uint8_t>(len) };
			return PrependRaw(b, sizeof(b));
		}
		return false;
	}

	bool PrependTag(uint8_t tag) { return PrependRaw(&tag, 1); }

	/** @brief Wraps everything written since `mark` (a previously-recorded `Cursor()`) as `tag`'s content. */
	bool WrapTLV(uint8_t tag, size_t contentLen)
	{
		if (!PrependLength(contentLen)) {
			return false;
		}
		return PrependTag(tag);
	}

	size_t Cursor() const { return mCursor; }
	const uint8_t *Data() const { return mBuffer + mCursor; }
	size_t Size() const { return mCapacity - mCursor; }

private:
	uint8_t *mBuffer;
	size_t mCapacity;
	size_t mCursor;
};

bool ParseDerInteger(DerReader &reader, std::array<uint8_t, 32> &out)
{
	const uint8_t *value{};
	size_t valueLen{};
	if (!reader.ReadTagLenValue(0x02, value, valueLen) || valueLen == 0 || valueLen > 33) {
		return false;
	}

	const uint8_t *realValue = value;
	size_t realLen = valueLen;
	if (valueLen == 33) {
		if (value[0] != 0x00) {
			return false;
		}
		realValue = value + 1;
		realLen = 32;
	}
	if (realLen > 32) {
		return false;
	}

	out.fill(0);
	memcpy(out.data() + (32 - realLen), realValue, realLen);
	return true;
}

/** @brief Parses a DER `ECDSA-Sig-Value ::= SEQUENCE { r INTEGER, s INTEGER }` into raw 64-byte `r || s`. */
bool ParseEcdsaDerSignature(const uint8_t *data, size_t len, Signature &outSignature)
{
	DerReader outer(data, len);
	const uint8_t *seq{};
	size_t seqLen{};
	if (!outer.ReadTagLenValue(0x30, seq, seqLen) || !outer.AtEnd()) {
		return false;
	}

	DerReader inner(seq, seqLen);
	std::array<uint8_t, 32> r{};
	std::array<uint8_t, 32> s{};
	if (!ParseDerInteger(inner, r) || !ParseDerInteger(inner, s) || !inner.AtEnd()) {
		return false;
	}

	memcpy(outSignature.data(), r.data(), r.size());
	memcpy(outSignature.data() + r.size(), s.data(), s.size());
	return true;
}

bool BuildName(DerWriter &writer, const uint8_t *commonName, size_t commonNameLen)
{
	if (commonNameLen == 0 || commonNameLen > 32) {
		return false;
	}

	const size_t mark = writer.Cursor();
	if (!writer.PrependRaw(commonName, commonNameLen)) {
		return false;
	}
	if (!writer.WrapTLV(0x0c, commonNameLen)) { /* UTF8String */
		return false;
	}

	static constexpr uint8_t kOidCommonName[]{ 0x06, 0x03, 0x55, 0x04, 0x03 };
	if (!writer.PrependRaw(kOidCommonName, sizeof(kOidCommonName))) {
		return false;
	}
	if (!writer.WrapTLV(0x30, mark - writer.Cursor())) { /* AttributeTypeAndValue */
		return false;
	}
	if (!writer.WrapTLV(0x31, mark - writer.Cursor())) { /* RelativeDistinguishedName (SET) */
		return false;
	}
	return writer.WrapTLV(0x30, mark - writer.Cursor()); /* Name (RDNSequence) */
}

bool BuildTime(DerWriter &writer, const uint8_t *value, size_t len)
{
	uint8_t tag{};
	if (len == 13) {
		tag = 0x17; /* UTCTime */
	} else if (len == 15) {
		tag = 0x18; /* GeneralizedTime */
	} else {
		return false;
	}

	if (!writer.PrependRaw(value, len)) {
		return false;
	}
	return writer.WrapTLV(tag, len);
}

bool BuildValidity(DerWriter &writer, const uint8_t *notBefore, size_t notBeforeLen, const uint8_t *notAfter,
		   size_t notAfterLen)
{
	const size_t mark = writer.Cursor();
	if (!BuildTime(writer, notAfter, notAfterLen)) {
		return false;
	}
	if (!BuildTime(writer, notBefore, notBeforeLen)) {
		return false;
	}
	return writer.WrapTLV(0x30, mark - writer.Cursor());
}

bool BuildSubjectPublicKeyInfo(DerWriter &writer, const uint8_t *publicKey65)
{
	const size_t mark = writer.Cursor();
	if (!writer.PrependRaw(publicKey65, kEccP256PublicKeyLength)) {
		return false;
	}
	const uint8_t kUnusedBits{ 0x00 };
	if (!writer.PrependRaw(&kUnusedBits, 1)) {
		return false;
	}
	if (!writer.WrapTLV(0x03, 1 + kEccP256PublicKeyLength)) { /* BIT STRING */
		return false;
	}

	/* AlgorithmIdentifier { id-ecPublicKey, prime256v1 } (fixed, whole TLV). */
	static constexpr uint8_t kAlgorithmIdentifier[]{ 0x30, 0x13, 0x06, 0x07, 0x2a, 0x86, 0x48, 0xce, 0x3d,
							 0x02, 0x01, 0x06, 0x08, 0x2a, 0x86, 0x48, 0xce, 0x3d,
							 0x03, 0x01, 0x07 };
	if (!writer.PrependRaw(kAlgorithmIdentifier, sizeof(kAlgorithmIdentifier))) {
		return false;
	}
	return writer.WrapTLV(0x30, mark - writer.Cursor());
}

bool BuildAuthorityKeyIdentifierExtension(DerWriter &writer, const uint8_t *aki)
{
	const size_t mark = writer.Cursor();
	if (!writer.PrependRaw(aki, kAkiLength)) {
		return false;
	}
	if (!writer.WrapTLV(0x80, kAkiLength)) { /* [0] keyIdentifier */
		return false;
	}
	if (!writer.WrapTLV(0x30, mark - writer.Cursor())) { /* AuthorityKeyIdentifier */
		return false;
	}
	if (!writer.WrapTLV(0x04, mark - writer.Cursor())) { /* extnValue OCTET STRING */
		return false;
	}

	static constexpr uint8_t kOidAuthorityKeyIdentifier[]{ 0x06, 0x03, 0x55, 0x1d, 0x23 };
	if (!writer.PrependRaw(kOidAuthorityKeyIdentifier, sizeof(kOidAuthorityKeyIdentifier))) {
		return false;
	}
	return writer.WrapTLV(0x30, mark - writer.Cursor()); /* Extension */
}

bool BuildExtensions(DerWriter &writer, const uint8_t *aki)
{
	const size_t mark = writer.Cursor();

	/* keyUsage: critical, digitalSignature bit only (fixed, whole Extension TLV). */
	static constexpr uint8_t kKeyUsageExtension[]{ 0x30, 0x0e, 0x06, 0x03, 0x55, 0x1d, 0x0f, 0x01,
						       0x01, 0xff, 0x04, 0x04, 0x03, 0x02, 0x07, 0x80 };
	if (!writer.PrependRaw(kKeyUsageExtension, sizeof(kKeyUsageExtension))) {
		return false;
	}

	/* basicConstraints: critical, cA absent/default-false (fixed, whole Extension TLV). */
	static constexpr uint8_t kBasicConstraintsExtension[]{ 0x30, 0x0c, 0x06, 0x03, 0x55, 0x1d,
							       0x13, 0x01, 0x01, 0xff, 0x04, 0x02,
							       0x30, 0x00 };
	if (!writer.PrependRaw(kBasicConstraintsExtension, sizeof(kBasicConstraintsExtension))) {
		return false;
	}

	if (!BuildAuthorityKeyIdentifierExtension(writer, aki)) {
		return false;
	}

	if (!writer.WrapTLV(0x30, mark - writer.Cursor())) { /* Extensions (SEQUENCE OF Extension) */
		return false;
	}
	return writer.WrapTLV(0xa3, mark - writer.Cursor()); /* [3] EXPLICIT */
}

bool BuildTbsCertificate(DerWriter &writer, const uint8_t *serial, size_t serialLen, const uint8_t *issuer,
			 size_t issuerLen, const uint8_t *notBefore, size_t notBeforeLen, const uint8_t *notAfter,
			 size_t notAfterLen, const uint8_t *subject, size_t subjectLen, const uint8_t *publicKey65,
			 const uint8_t *aki)
{
	const size_t mark = writer.Cursor();

	if (!BuildExtensions(writer, aki)) {
		return false;
	}
	if (!BuildSubjectPublicKeyInfo(writer, publicKey65)) {
		return false;
	}
	if (!BuildName(writer, subject, subjectLen)) {
		return false;
	}
	if (!BuildValidity(writer, notBefore, notBeforeLen, notAfter, notAfterLen)) {
		return false;
	}
	if (!BuildName(writer, issuer, issuerLen)) {
		return false;
	}

	/* signature: AlgorithmIdentifier { ecdsa-with-SHA256 } (fixed, whole TLV). */
	static constexpr uint8_t kSignatureAlgorithmIdentifier[]{ 0x30, 0x0a, 0x06, 0x08, 0x2a,
								  0x86, 0x48, 0xce, 0x3d, 0x04,
								  0x03, 0x02 };
	if (!writer.PrependRaw(kSignatureAlgorithmIdentifier, sizeof(kSignatureAlgorithmIdentifier))) {
		return false;
	}

	if (!writer.PrependRaw(serial, serialLen)) {
		return false;
	}
	if (!writer.WrapTLV(0x02, serialLen)) { /* serialNumber INTEGER */
		return false;
	}

	/* version: [0] EXPLICIT INTEGER 2 (v3) (fixed, whole TLV). */
	static constexpr uint8_t kVersion[]{ 0xa0, 0x03, 0x02, 0x01, 0x02 };
	if (!writer.PrependRaw(kVersion, sizeof(kVersion))) {
		return false;
	}

	return writer.WrapTLV(0x30, mark - writer.Cursor());
}

} // namespace

AliroError Validate(ConstData certificate, const PublicKey &issuerPublicKey, PublicKey &outSubjectPublicKey)
{
	outSubjectPublicKey.fill(0);

	if (certificate.mData == nullptr || certificate.mLength == 0 ||
	    certificate.mLength > kMaxProfile0000Length) {
		return ALIRO_INVALID_DATA_FORMAT;
	}

	DerReader outer(certificate.mData, certificate.mLength);
	const uint8_t *profileSeq{};
	size_t profileSeqLen{};
	if (!outer.ReadTagLenValue(0x30, profileSeq, profileSeqLen) || !outer.AtEnd()) {
		return ALIRO_INVALID_DATA_FORMAT;
	}

	DerReader profileReader(profileSeq, profileSeqLen);
	const uint8_t *profileTag{};
	size_t profileTagLen{};
	if (!profileReader.ReadTagLenValue(0x04, profileTag, profileTagLen)) {
		return ALIRO_INVALID_DATA_FORMAT;
	}
	if (profileTagLen != 2 || profileTag[0] != 0x00 || profileTag[1] != 0x00) {
		LOG_WRN("Unsupported certificate compression profile");
		return ALIRO_INVALID_DATA_FORMAT;
	}

	const uint8_t *dataSeq{};
	size_t dataSeqLen{};
	if (!profileReader.ReadTagLenValue(0x30, dataSeq, dataSeqLen) || !profileReader.AtEnd()) {
		return ALIRO_INVALID_DATA_FORMAT;
	}

	DerReader fieldReader(dataSeq, dataSeqLen);

	static constexpr uint8_t kDefaultSerial[]{ 0x01 };
	static constexpr uint8_t kDefaultIssuer[]{ 'i', 's', 's', 'u', 'e', 'r' };
	static constexpr uint8_t kDefaultSubject[]{ 's', 'u', 'b', 'j', 'e', 'c', 't' };
	static constexpr uint8_t kDefaultNotBefore[]{ '2', '0', '0', '1', '0', '1', '0',
						      '0', '0', '0', '0', '0', 'Z' };
	static constexpr uint8_t kDefaultNotAfter[]{ '4', '9', '0', '1', '0', '1', '0',
						     '0', '0', '0', '0', '0', 'Z' };

	const uint8_t *serial = kDefaultSerial;
	size_t serialLen = sizeof(kDefaultSerial);
	const uint8_t *issuer = kDefaultIssuer;
	size_t issuerLen = sizeof(kDefaultIssuer);
	const uint8_t *notBefore = kDefaultNotBefore;
	size_t notBeforeLen = sizeof(kDefaultNotBefore);
	const uint8_t *notAfter = kDefaultNotAfter;
	size_t notAfterLen = sizeof(kDefaultNotAfter);
	const uint8_t *subject = kDefaultSubject;
	size_t subjectLen = sizeof(kDefaultSubject);

	const auto tryOptional = [&](uint8_t tag, const uint8_t *&outPtr, size_t &outLen, size_t minLen,
				     size_t maxLen) -> bool {
		uint8_t peek{};
		if (!fieldReader.PeekTag(peek) || peek != tag) {
			return true; /* absent: default already set */
		}
		const uint8_t *value{};
		size_t valueLen{};
		if (!fieldReader.ReadTagLenValue(tag, value, valueLen)) {
			return false;
		}
		if (valueLen < minLen || valueLen > maxLen) {
			return false;
		}
		outPtr = value;
		outLen = valueLen;
		return true;
	};

	if (!tryOptional(0x80, serial, serialLen, 1, 20) || !tryOptional(0x81, issuer, issuerLen, 1, 32) ||
	    !tryOptional(0x82, notBefore, notBeforeLen, 13, 15) ||
	    !tryOptional(0x83, notAfter, notAfterLen, 13, 15) ||
	    !tryOptional(0x84, subject, subjectLen, 1, 32)) {
		return ALIRO_INVALID_DATA_FORMAT;
	}

	const uint8_t *publicKeyField{};
	size_t publicKeyFieldLen{};
	if (!fieldReader.ReadTagLenValue(0x85, publicKeyField, publicKeyFieldLen)) {
		return ALIRO_INVALID_DATA_FORMAT;
	}
	if (publicKeyFieldLen != 1 + kEccP256PublicKeyLength || publicKeyField[0] != 0x00) {
		return ALIRO_INVALID_DATA_FORMAT;
	}

	const uint8_t *signatureField{};
	size_t signatureFieldLen{};
	if (!fieldReader.ReadTagLenValue(0x86, signatureField, signatureFieldLen)) {
		return ALIRO_INVALID_DATA_FORMAT;
	}
	if (signatureFieldLen < 2 || signatureFieldLen > 76 || signatureField[0] != 0x00) {
		return ALIRO_INVALID_DATA_FORMAT;
	}

	if (!fieldReader.AtEnd()) {
		return ALIRO_INVALID_DATA_FORMAT;
	}

	Signature rawSignature{};
	if (!ParseEcdsaDerSignature(signatureField + 1, signatureFieldLen - 1, rawSignature)) {
		return ALIRO_INVALID_DATA_FORMAT;
	}

	std::array<uint8_t, kAkiLength> aki{};
	{
		size_t hashLen{};
		const psa_status_t status = psa_hash_compute(PSA_ALG_SHA_1, issuerPublicKey.data(),
							      issuerPublicKey.size(), aki.data(), aki.size(),
							      &hashLen);
		if (status != PSA_SUCCESS || hashLen != aki.size()) {
			LOG_ERR("psa_hash_compute(SHA-1) failed: %d", status);
			return ALIRO_ERROR_INTERNAL;
		}
	}

	std::array<uint8_t, kMaxTbsBufferLength> tbsBuffer{};
	DerWriter writer(tbsBuffer.data(), tbsBuffer.size());
	if (!BuildTbsCertificate(writer, serial, serialLen, issuer, issuerLen, notBefore, notBeforeLen, notAfter,
				 notAfterLen, subject, subjectLen, publicKeyField + 1, aki.data())) {
		LOG_ERR("Failed to reconstruct TBSCertificate");
		return ALIRO_ERROR_INTERNAL;
	}

	psa_key_attributes_t attributes = PSA_KEY_ATTRIBUTES_INIT;
	psa_set_key_type(&attributes, PSA_KEY_TYPE_ECC_PUBLIC_KEY(PSA_ECC_FAMILY_SECP_R1));
	psa_set_key_algorithm(&attributes, PSA_ALG_ECDSA(PSA_ALG_SHA_256));
	psa_set_key_bits(&attributes, PSA_BYTES_TO_BITS(::Aliro::CryptoTypes::kEccP256KeyPrivateKeyLength));
	psa_set_key_usage_flags(&attributes, PSA_KEY_USAGE_VERIFY_HASH);

	psa_key_id_t verifyKeyId{};
	const psa_status_t importStatus =
		psa_import_key(&attributes, issuerPublicKey.data(), issuerPublicKey.size(), &verifyKeyId);
	if (importStatus != PSA_SUCCESS) {
		LOG_ERR("psa_import_key(issuer public key) failed: %d", importStatus);
		return ALIRO_ERROR_INTERNAL;
	}

	const psa_status_t verifyStatus =
		psa_verify_message(verifyKeyId, PSA_ALG_ECDSA(PSA_ALG_SHA_256), writer.Data(), writer.Size(),
				   rawSignature.data(), rawSignature.size());
	psa_destroy_key(verifyKeyId);

	if (verifyStatus != PSA_SUCCESS) {
		return ALIRO_INVALID_SIGNATURE;
	}

	memcpy(outSubjectPublicKey.data(), publicKeyField + 1, outSubjectPublicKey.size());
	return ALIRO_NO_ERROR;
}

} // namespace AliroUd::Crypto::Certificate
