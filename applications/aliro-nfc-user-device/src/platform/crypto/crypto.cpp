/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include "certificate.h"

#include <aliro/user_device/interface.h>

#include <psa/crypto.h>
#include <zephyr/init.h>
#include <zephyr/logging/log.h>

#include <array>
#include <cstring>

LOG_MODULE_REGISTER(aliro_ud_crypto, CONFIG_ALIRO_UD_CRYPTO_LOG_LEVEL);

/*
 * Aliro::Interface::UserDevice::Crypto contract (APP_PLAN.md AWP5): thin PSA
 * Crypto bindings for ordinary key/cipher operations, plus the
 * application-owned profile0000 certificate validation pipeline
 * (certificate.cpp). Aliro-specific KDF construction and protocol
 * sequencing remain stack-owned; this module only exposes the generic
 * primitives the stack's AUTH0/AUTH1/EXCHANGE orchestration composes them
 * with.
 *
 * Every key here (ephemeral ECDH pairs, derived symmetric keys) is
 * PSA_KEY_LIFETIME_VOLATILE: Phase 1 never persists a session/ephemeral key
 * across a session, so no application-chosen persistent key ID scheme is
 * needed here (contrast `storage/credential/key_backend_psa.cpp`, which
 * imports the long-lived Access Credential private key at an explicit
 * persistent ID). This also means the same implementation runs unchanged on
 * native_sim (host tests) and the DK: unlike persistent/trusted key storage,
 * ordinary PSA volatile-key crypto operations need no hardware-backed
 * secure-storage driver.
 */

using namespace Aliro;
using namespace Aliro::CryptoTypes;

namespace {

constexpr KeyId kImportedKeyMarker{ 0x80000000u };
constexpr size_t kMaxImportedKeys{ 4 };

struct ImportedKeySlot {
	psa_key_id_t deriveKeyId{};
	psa_key_id_t aeadKeyId{};
};

std::array<ImportedKeySlot, kMaxImportedKeys> gImportedKeySlots{};

int InitPsaCrypto(void)
{
	const psa_status_t status = psa_crypto_init();

	if (status != PSA_SUCCESS && status != PSA_ERROR_ALREADY_EXISTS) {
		LOG_ERR("psa_crypto_init() failed: %d", status);
	}

	return 0;
}

SYS_INIT(InitPsaCrypto, APPLICATION, CONFIG_KERNEL_INIT_PRIORITY_DEFAULT);

bool IsImportedKey(KeyId keyId)
{
	return (keyId & kImportedKeyMarker) != 0;
}

size_t ImportedKeySlotIndex(KeyId keyId)
{
	return static_cast<size_t>(keyId & ~kImportedKeyMarker);
}

psa_key_id_t ResolveDeriveKeyId(KeyId keyId)
{
	if (!IsImportedKey(keyId)) {
		return static_cast<psa_key_id_t>(keyId);
	}

	const size_t index = ImportedKeySlotIndex(keyId);
	if (index >= kMaxImportedKeys) {
		return 0;
	}

	return gImportedKeySlots[index].deriveKeyId;
}

psa_key_id_t ResolveAeadKeyId(KeyId keyId)
{
	if (!IsImportedKey(keyId)) {
		return static_cast<psa_key_id_t>(keyId);
	}

	const size_t index = ImportedKeySlotIndex(keyId);
	if (index >= kMaxImportedKeys) {
		return 0;
	}

	return gImportedKeySlots[index].aeadKeyId;
}

psa_key_attributes_t GetImportedDeriveKeyAttributes(size_t keyMaterialLength)
{
	psa_key_attributes_t attributes = PSA_KEY_ATTRIBUTES_INIT;

	psa_set_key_type(&attributes, PSA_KEY_TYPE_DERIVE);
	psa_set_key_algorithm(&attributes, PSA_ALG_HKDF(PSA_ALG_SHA_256));
	psa_set_key_bits(&attributes, PSA_BYTES_TO_BITS(keyMaterialLength));
	psa_set_key_usage_flags(&attributes, PSA_KEY_USAGE_DERIVE);

	return attributes;
}

psa_key_attributes_t GetImportedAeadKeyAttributes(size_t keyMaterialLength)
{
	psa_key_attributes_t attributes = PSA_KEY_ATTRIBUTES_INIT;

	psa_set_key_type(&attributes, PSA_KEY_TYPE_AES);
	psa_set_key_algorithm(&attributes, PSA_ALG_GCM);
	psa_set_key_bits(&attributes, PSA_BYTES_TO_BITS(keyMaterialLength));
	psa_set_key_usage_flags(&attributes, PSA_KEY_USAGE_ENCRYPT | PSA_KEY_USAGE_DECRYPT);

	return attributes;
}

AliroError DestroyImportedKeySlot(size_t index)
{
	if (index >= kMaxImportedKeys) {
		return ALIRO_ERROR_INTERNAL;
	}

	ImportedKeySlot &slot = gImportedKeySlots[index];

	if (slot.deriveKeyId != 0) {
		const psa_status_t status = psa_destroy_key(slot.deriveKeyId);
		if (status != PSA_SUCCESS && status != PSA_ERROR_INVALID_HANDLE) {
			LOG_ERR("psa_destroy_key(derive 0x%08x) failed: %d", slot.deriveKeyId, status);
			return ALIRO_ERROR_INTERNAL;
		}
	}

	if (slot.aeadKeyId != 0) {
		const psa_status_t status = psa_destroy_key(slot.aeadKeyId);
		if (status != PSA_SUCCESS && status != PSA_ERROR_INVALID_HANDLE) {
			LOG_ERR("psa_destroy_key(aead 0x%08x) failed: %d", slot.aeadKeyId, status);
			return ALIRO_ERROR_INTERNAL;
		}
	}

	slot = {};
	return ALIRO_NO_ERROR;
}

} // namespace

namespace Aliro::Interface::UserDevice::Crypto {

AliroError GenerateRandom(uint8_t *buffer, size_t bufferLength)
{
	if (buffer == nullptr && bufferLength > 0) {
		return ALIRO_INVALID_ARGUMENT;
	}
	if (bufferLength == 0) {
		return ALIRO_NO_ERROR;
	}

	const psa_status_t status = psa_generate_random(buffer, bufferLength);
	if (status != PSA_SUCCESS) {
		LOG_ERR("psa_generate_random() failed: %d", status);
		return ALIRO_ERROR_INTERNAL;
	}

	return ALIRO_NO_ERROR;
}

AliroError GenerateEphemeralKeyPair(KeyId &outKeyId, PublicKey &outPublicKey)
{
	outKeyId = 0;
	outPublicKey.fill(0);

	psa_key_attributes_t attributes = PSA_KEY_ATTRIBUTES_INIT;
	psa_set_key_type(&attributes, PSA_KEY_TYPE_ECC_KEY_PAIR(PSA_ECC_FAMILY_SECP_R1));
	psa_set_key_algorithm(&attributes, PSA_ALG_ECDH);
	psa_set_key_bits(&attributes, PSA_BYTES_TO_BITS(kEccP256KeyPrivateKeyLength));
	psa_set_key_usage_flags(&attributes, PSA_KEY_USAGE_DERIVE);

	psa_key_id_t keyId{};
	const psa_status_t genStatus = psa_generate_key(&attributes, &keyId);
	if (genStatus != PSA_SUCCESS) {
		LOG_ERR("psa_generate_key() failed: %d", genStatus);
		return ALIRO_ERROR_INTERNAL;
	}

	size_t outLen{};
	const psa_status_t exportStatus =
		psa_export_public_key(keyId, outPublicKey.data(), outPublicKey.size(), &outLen);
	if (exportStatus != PSA_SUCCESS || outLen != outPublicKey.size()) {
		LOG_ERR("psa_export_public_key() failed: %d", exportStatus);
		psa_destroy_key(keyId);
		return ALIRO_ERROR_INTERNAL;
	}

	outKeyId = keyId;
	return ALIRO_NO_ERROR;
}

AliroError RawKeyAgreement(KeyId keyId, const PublicKey &peerPublicKey, SharedSecret &outSharedSecret)
{
	size_t outLen{};
	const psa_status_t status = psa_raw_key_agreement(PSA_ALG_ECDH, keyId, peerPublicKey.data(),
							  peerPublicKey.size(), outSharedSecret.data(),
							  outSharedSecret.size(), &outLen);
	if (status != PSA_SUCCESS || outLen != outSharedSecret.size()) {
		LOG_WRN("psa_raw_key_agreement() failed: %d", status);
		return ALIRO_ERROR_INTERNAL;
	}

	return ALIRO_NO_ERROR;
}

namespace {

AliroError RunHkdfDerivation(KeyId keyId, const uint8_t *info, size_t infoLength, const uint8_t *salt,
			     size_t saltLength, psa_key_derivation_operation_t &operation)
{
	operation = PSA_KEY_DERIVATION_OPERATION_INIT;

	psa_status_t status = psa_key_derivation_setup(&operation, PSA_ALG_HKDF(PSA_ALG_SHA_256));
	if (status != PSA_SUCCESS) {
		LOG_ERR("psa_key_derivation_setup() failed: %d", status);
		return ALIRO_ERROR_INTERNAL;
	}

	status = psa_key_derivation_input_bytes(&operation, PSA_KEY_DERIVATION_INPUT_SALT, salt, saltLength);
	if (status != PSA_SUCCESS) {
		LOG_ERR("psa_key_derivation_input_bytes(SALT) failed: %d", status);
		psa_key_derivation_abort(&operation);
		return ALIRO_ERROR_INTERNAL;
	}

	status = psa_key_derivation_input_key(&operation, PSA_KEY_DERIVATION_INPUT_SECRET,
					      ResolveDeriveKeyId(keyId));
	if (status != PSA_SUCCESS) {
		LOG_ERR("psa_key_derivation_input_key(SECRET) failed: %d", status);
		psa_key_derivation_abort(&operation);
		return ALIRO_ERROR_INTERNAL;
	}

	status = psa_key_derivation_input_bytes(&operation, PSA_KEY_DERIVATION_INPUT_INFO, info, infoLength);
	if (status != PSA_SUCCESS) {
		LOG_ERR("psa_key_derivation_input_bytes(INFO) failed: %d", status);
		psa_key_derivation_abort(&operation);
		return ALIRO_ERROR_INTERNAL;
	}

	return ALIRO_NO_ERROR;
}

} // namespace

AliroError DeriveSymmetricKey(KeyId keyId, const uint8_t *info, size_t infoLength, const uint8_t *salt,
			      size_t saltLength, KeyId &outKeyId)
{
	outKeyId = 0;

	psa_key_derivation_operation_t operation{};
	AliroError error = RunHkdfDerivation(keyId, info, infoLength, salt, saltLength, operation);
	if (error != ALIRO_NO_ERROR) {
		return error;
	}

	psa_key_attributes_t attributes = PSA_KEY_ATTRIBUTES_INIT;
	psa_set_key_type(&attributes, PSA_KEY_TYPE_AES);
	psa_set_key_algorithm(&attributes, PSA_ALG_GCM);
	psa_set_key_bits(&attributes, PSA_BYTES_TO_BITS(kSymmetricKeyLength));
	psa_set_key_usage_flags(&attributes, PSA_KEY_USAGE_ENCRYPT | PSA_KEY_USAGE_DECRYPT);

	psa_key_id_t derivedKeyId{};
	const psa_status_t status = psa_key_derivation_output_key(&attributes, &operation, &derivedKeyId);
	psa_key_derivation_abort(&operation);

	if (status != PSA_SUCCESS) {
		LOG_ERR("psa_key_derivation_output_key() failed: %d", status);
		return ALIRO_ERROR_INTERNAL;
	}

	outKeyId = derivedKeyId;
	return ALIRO_NO_ERROR;
}

AliroError DeriveRawKey(KeyId keyId, const uint8_t *info, size_t infoLength, const uint8_t *salt, size_t saltLength,
			uint8_t *outKey, size_t outKeyLength)
{
	psa_key_derivation_operation_t operation{};
	AliroError error = RunHkdfDerivation(keyId, info, infoLength, salt, saltLength, operation);
	if (error != ALIRO_NO_ERROR) {
		return error;
	}

	const psa_status_t status = psa_key_derivation_output_bytes(&operation, outKey, outKeyLength);
	psa_key_derivation_abort(&operation);

	if (status != PSA_SUCCESS) {
		LOG_ERR("psa_key_derivation_output_bytes() failed: %d", status);
		return ALIRO_ERROR_INTERNAL;
	}

	return ALIRO_NO_ERROR;
}

namespace {
/* Generous bound covering the largest plaintext this application ever AEADs
 * (Access Protocol payloads and mailbox EXCHANGE data, both far below this).
 */
constexpr size_t kMaxAeadPlainTextLength{ 512 };
} // namespace

AliroError AeadEncrypt(KeyId keyId, const uint8_t *plainText, size_t plainTextLength, const uint8_t *additionalData,
		       size_t additionalDataLength, const Nonce &nonce, uint8_t *outCipherText,
		       AuthenticationTag &outAuthTag)
{
	/* AES-GCM: ciphertext-with-tag output buffer is plainTextLength + tag size; split for the caller. */
	std::array<uint8_t, PSA_AEAD_ENCRYPT_OUTPUT_SIZE(PSA_KEY_TYPE_AES, PSA_ALG_GCM, kMaxAeadPlainTextLength)>
		scratch{};
	if (plainTextLength > kMaxAeadPlainTextLength) {
		return ALIRO_INVALID_ARGUMENT;
	}

	size_t outLen{};
	const psa_key_id_t psaKeyId = ResolveAeadKeyId(keyId);
	const psa_status_t status =
		psa_aead_encrypt(psaKeyId, PSA_ALG_GCM, nonce.data(), nonce.size(), additionalData, additionalDataLength,
				 plainText, plainTextLength, scratch.data(), scratch.size(), &outLen);
	if (status != PSA_SUCCESS || outLen != plainTextLength + outAuthTag.size()) {
		LOG_WRN("psa_aead_encrypt() failed: %d", status);
		return ALIRO_ERROR_INTERNAL;
	}

	memcpy(outCipherText, scratch.data(), plainTextLength);
	memcpy(outAuthTag.data(), scratch.data() + plainTextLength, outAuthTag.size());
	return ALIRO_NO_ERROR;
}

AliroError AeadDecrypt(KeyId keyId, const uint8_t *cipherTextWithTag, size_t cipherTextWithTagLength,
		       const uint8_t *additionalData, size_t additionalDataLength, const Nonce &nonce,
		       uint8_t *outPlainText, size_t &plainTextLength)
{
	size_t outLen{};
	const psa_key_id_t psaKeyId = ResolveAeadKeyId(keyId);
	const psa_status_t status =
		psa_aead_decrypt(psaKeyId, PSA_ALG_GCM, nonce.data(), nonce.size(), additionalData, additionalDataLength,
				 cipherTextWithTag, cipherTextWithTagLength, outPlainText, plainTextLength, &outLen);
	if (status != PSA_SUCCESS) {
		return status == PSA_ERROR_INVALID_SIGNATURE ? ALIRO_INVALID_AUTHENTICATION_TAG
							      : ALIRO_ERROR_INTERNAL;
	}

	plainTextLength = outLen;
	return ALIRO_NO_ERROR;
}

AliroError VerifySignature(const PublicKey &publicKey, const uint8_t *msg, size_t msgLength,
			   const Signature &signature)
{
	psa_key_attributes_t attributes = PSA_KEY_ATTRIBUTES_INIT;
	psa_set_key_type(&attributes, PSA_KEY_TYPE_ECC_PUBLIC_KEY(PSA_ECC_FAMILY_SECP_R1));
	psa_set_key_algorithm(&attributes, PSA_ALG_ECDSA(PSA_ALG_SHA_256));
	psa_set_key_bits(&attributes, PSA_BYTES_TO_BITS(kEccP256KeyPrivateKeyLength));
	psa_set_key_usage_flags(&attributes, PSA_KEY_USAGE_VERIFY_HASH);

	psa_key_id_t keyId{};
	const psa_status_t importStatus = psa_import_key(&attributes, publicKey.data(), publicKey.size(), &keyId);
	if (importStatus != PSA_SUCCESS) {
		LOG_WRN("psa_import_key(public key) failed: %d", importStatus);
		return ALIRO_INVALID_SIGNATURE;
	}

	const psa_status_t verifyStatus = psa_verify_message(keyId, PSA_ALG_ECDSA(PSA_ALG_SHA_256), msg, msgLength,
							     signature.data(), signature.size());
	psa_destroy_key(keyId);

	return verifyStatus == PSA_SUCCESS ? ALIRO_NO_ERROR : ALIRO_INVALID_SIGNATURE;
}

AliroError Sha256(const uint8_t *data, size_t dataLength, Sha256Hash &outHash)
{
	size_t outLen{};
	const psa_status_t status =
		psa_hash_compute(PSA_ALG_SHA_256, data, dataLength, outHash.data(), outHash.size(), &outLen);
	if (status != PSA_SUCCESS || outLen != outHash.size()) {
		LOG_ERR("psa_hash_compute(SHA-256) failed: %d", status);
		return ALIRO_ERROR_INTERNAL;
	}

	return ALIRO_NO_ERROR;
}

AliroError Sha1(const uint8_t *data, size_t dataLength, Sha1Hash &outHash)
{
	size_t outLen{};
	const psa_status_t status =
		psa_hash_compute(PSA_ALG_SHA_1, data, dataLength, outHash.data(), outHash.size(), &outLen);
	if (status != PSA_SUCCESS || outLen != outHash.size()) {
		LOG_ERR("psa_hash_compute(SHA-1) failed: %d", status);
		return ALIRO_ERROR_INTERNAL;
	}

	return ALIRO_NO_ERROR;
}

AliroError ValidateCertificate(ConstData certificate, const PublicKey &issuerPublicKey, PublicKey &outSubjectPublicKey)
{
	return AliroUd::Crypto::Certificate::Validate(certificate, issuerPublicKey, outSubjectPublicKey);
}

AliroError DestroyKey(KeyId &keyId)
{
	if (keyId == 0) {
		return ALIRO_NO_ERROR;
	}

	if (IsImportedKey(keyId)) {
		const AliroError error = DestroyImportedKeySlot(ImportedKeySlotIndex(keyId));
		keyId = 0;
		return error;
	}

	const psa_status_t status = psa_destroy_key(keyId);
	if (status != PSA_SUCCESS && status != PSA_ERROR_INVALID_HANDLE) {
		LOG_ERR("psa_destroy_key(0x%08x) failed: %d", keyId, status);
		return ALIRO_ERROR_INTERNAL;
	}

	keyId = 0;
	return ALIRO_NO_ERROR;
}

AliroError ImportKey(const uint8_t *keyMaterial, size_t keyMaterialLength, KeyId &outKeyId)
{
	outKeyId = 0;

	if (keyMaterial == nullptr || keyMaterialLength == 0) {
		return ALIRO_INVALID_ARGUMENT;
	}

	size_t slotIndex{ kMaxImportedKeys };
	for (size_t i = 0; i < kMaxImportedKeys; ++i) {
		if (gImportedKeySlots[i].deriveKeyId == 0 && gImportedKeySlots[i].aeadKeyId == 0) {
			slotIndex = i;
			break;
		}
	}
	if (slotIndex >= kMaxImportedKeys) {
		LOG_ERR("No free ImportKey slot");
		return ALIRO_ERROR_INTERNAL;
	}

	psa_key_attributes_t deriveAttributes = GetImportedDeriveKeyAttributes(keyMaterialLength);
	psa_key_id_t deriveKeyId{};
	const psa_status_t deriveStatus =
		psa_import_key(&deriveAttributes, keyMaterial, keyMaterialLength, &deriveKeyId);
	if (deriveStatus != PSA_SUCCESS) {
		LOG_ERR("psa_import_key(derive) failed: %d", deriveStatus);
		return ALIRO_ERROR_INTERNAL;
	}

	psa_key_attributes_t aeadAttributes = GetImportedAeadKeyAttributes(keyMaterialLength);
	psa_key_id_t aeadKeyId{};
	const psa_status_t aeadStatus = psa_import_key(&aeadAttributes, keyMaterial, keyMaterialLength, &aeadKeyId);
	if (aeadStatus != PSA_SUCCESS) {
		LOG_ERR("psa_import_key(aead) failed: %d", aeadStatus);
		psa_destroy_key(deriveKeyId);
		return ALIRO_ERROR_INTERNAL;
	}

	gImportedKeySlots[slotIndex].deriveKeyId = deriveKeyId;
	gImportedKeySlots[slotIndex].aeadKeyId = aeadKeyId;
	outKeyId = kImportedKeyMarker | static_cast<KeyId>(slotIndex);
	return ALIRO_NO_ERROR;
}

} // namespace Aliro::Interface::UserDevice::Crypto
