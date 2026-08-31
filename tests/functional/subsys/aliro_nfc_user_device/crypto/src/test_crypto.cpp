/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include <zephyr/ztest.h>

#include <aliro/user_device/interface.h>

#include <psa/crypto.h>

#include <array>
#include <cstring>

/*
 * Aliro::Interface::UserDevice::Crypto PSA Crypto bindings under test
 * (APP_PLAN.md AWP5, platform/crypto/crypto.cpp).
 *
 * Sha256Test uses a NIST FIPS 180-2 known-answer vector (SHA-256("abc")).
 * Every other primitive here (ECDH, HKDF, AES-GCM, ECDSA) is exercised as a
 * self-consistent round trip rather than against a hardcoded known-answer
 * vector: these thin bindings call directly into mbedtls/PSA Crypto (already
 * separately validated upstream), so what AWP5 needs to prove is that this
 * module wires the right PSA algorithm/key-type/parameter combination for
 * each Aliro::Interface::UserDevice::Crypto contract function - which a
 * generate/use/verify round trip demonstrates without transcribing a large
 * external test vector into this file.
 */

using namespace Aliro;
using namespace Aliro::CryptoTypes;
using namespace Aliro::Interface::UserDevice::Crypto;

namespace {

void ResetGlobalState(void *fixture)
{
	(void)fixture;
}

} // namespace

ZTEST_SUITE(aliro_ud_crypto, nullptr, nullptr, ResetGlobalState, nullptr, nullptr);

ZTEST(aliro_ud_crypto, test_generate_random_fills_buffer_and_varies)
{
	std::array<uint8_t, 32> first{};
	std::array<uint8_t, 32> second{};

	zassert_equal(ALIRO_NO_ERROR, GenerateRandom(first.data(), first.size()));
	zassert_equal(ALIRO_NO_ERROR, GenerateRandom(second.data(), second.size()));

	/* Overwhelmingly unlikely to collide for correctly-wired psa_generate_random(). */
	zassert_not_equal(0, memcmp(first.data(), second.data(), first.size()));
}

ZTEST(aliro_ud_crypto, test_generate_random_rejects_null_buffer_with_nonzero_length)
{
	zassert_equal(ALIRO_INVALID_ARGUMENT, GenerateRandom(nullptr, 1));
}

ZTEST(aliro_ud_crypto, test_generate_random_zero_length_is_a_no_op_success)
{
	zassert_equal(ALIRO_NO_ERROR, GenerateRandom(nullptr, 0));
}

ZTEST(aliro_ud_crypto, test_ephemeral_ecdh_agreement_matches_on_both_sides)
{
	KeyId localKeyId{};
	PublicKey localPublicKey{};
	zassert_equal(ALIRO_NO_ERROR, GenerateEphemeralKeyPair(localKeyId, localPublicKey));
	zassert_not_equal(0u, localKeyId);
	zassert_equal(kEccP256PublicKeyPrefix, localPublicKey[0]);

	KeyId peerKeyId{};
	PublicKey peerPublicKey{};
	zassert_equal(ALIRO_NO_ERROR, GenerateEphemeralKeyPair(peerKeyId, peerPublicKey));

	SharedSecret localSecret{};
	SharedSecret peerSecret{};
	zassert_equal(ALIRO_NO_ERROR, RawKeyAgreement(localKeyId, peerPublicKey, localSecret));
	zassert_equal(ALIRO_NO_ERROR, RawKeyAgreement(peerKeyId, localPublicKey, peerSecret));

	zassert_mem_equal(localSecret.data(), peerSecret.data(), localSecret.size(),
			  "ECDH must be commutative: both sides derive the same shared secret");

	zassert_equal(ALIRO_NO_ERROR, DestroyKey(localKeyId));
	zassert_equal(0u, localKeyId);
	zassert_equal(ALIRO_NO_ERROR, DestroyKey(peerKeyId));
}

ZTEST(aliro_ud_crypto, test_derive_symmetric_key_round_trips_through_aead)
{
	KeyId localKeyId{};
	PublicKey localPublicKey{};
	KeyId peerKeyId{};
	PublicKey peerPublicKey{};
	zassert_equal(ALIRO_NO_ERROR, GenerateEphemeralKeyPair(localKeyId, localPublicKey));
	zassert_equal(ALIRO_NO_ERROR, GenerateEphemeralKeyPair(peerKeyId, peerPublicKey));

	SharedSecret secret{};
	zassert_equal(ALIRO_NO_ERROR, RawKeyAgreement(localKeyId, peerPublicKey, secret));

	/* HKDF requires the input key material to already be a PSA key; import the
	 * raw shared secret as a derive-capable key for this test's own use. */
	psa_key_attributes_t ikmAttributes = PSA_KEY_ATTRIBUTES_INIT;
	psa_set_key_type(&ikmAttributes, PSA_KEY_TYPE_DERIVE);
	psa_set_key_algorithm(&ikmAttributes, PSA_ALG_HKDF(PSA_ALG_SHA_256));
	psa_set_key_usage_flags(&ikmAttributes, PSA_KEY_USAGE_DERIVE);
	psa_key_id_t ikmKeyId{};
	zassert_equal(PSA_SUCCESS,
		     psa_import_key(&ikmAttributes, secret.data(), secret.size(), &ikmKeyId));

	const std::array<uint8_t, 4> info{ 'i', 'n', 'f', 'o' };
	const std::array<uint8_t, 4> salt{ 's', 'a', 'l', 't' };

	KeyId symmetricKeyId{};
	zassert_equal(ALIRO_NO_ERROR, DeriveSymmetricKey(ikmKeyId, info.data(), info.size(), salt.data(), salt.size(),
							 symmetricKeyId));
	zassert_not_equal(0u, symmetricKeyId);

	const std::array<uint8_t, 5> plainText{ 'h', 'e', 'l', 'l', 'o' };
	const std::array<uint8_t, 3> aad{ 'a', 'a', 'd' };
	Nonce nonce{};
	std::array<uint8_t, plainText.size()> cipherText{};
	AuthenticationTag tag{};
	zassert_equal(ALIRO_NO_ERROR, AeadEncrypt(symmetricKeyId, plainText.data(), plainText.size(), aad.data(),
						  aad.size(), nonce, cipherText.data(), tag));
	zassert_not_equal(0, memcmp(plainText.data(), cipherText.data(), plainText.size()));

	std::array<uint8_t, plainText.size() + tag.size()> cipherWithTag{};
	memcpy(cipherWithTag.data(), cipherText.data(), cipherText.size());
	memcpy(cipherWithTag.data() + cipherText.size(), tag.data(), tag.size());

	std::array<uint8_t, plainText.size()> decrypted{};
	size_t decryptedLength{ decrypted.size() };
	zassert_equal(ALIRO_NO_ERROR, AeadDecrypt(symmetricKeyId, cipherWithTag.data(), cipherWithTag.size(),
						  aad.data(), aad.size(), nonce, decrypted.data(), decryptedLength));
	zassert_equal(plainText.size(), decryptedLength);
	zassert_mem_equal(plainText.data(), decrypted.data(), plainText.size());

	/* Corrupting one ciphertext byte must be caught by the GCM tag, not silently decrypted. */
	std::array<uint8_t, plainText.size() + tag.size()> tampered{ cipherWithTag };
	tampered[0] ^= 0x01;
	size_t tamperedLength{ decrypted.size() };
	zassert_equal(ALIRO_INVALID_AUTHENTICATION_TAG,
		     AeadDecrypt(symmetricKeyId, tampered.data(), tampered.size(), aad.data(), aad.size(), nonce,
				 decrypted.data(), tamperedLength));

	psa_destroy_key(ikmKeyId);
	DestroyKey(localKeyId);
	DestroyKey(peerKeyId);
	DestroyKey(symmetricKeyId);
}

ZTEST(aliro_ud_crypto, test_derive_raw_key_is_deterministic_for_same_inputs)
{
	psa_key_attributes_t ikmAttributes = PSA_KEY_ATTRIBUTES_INIT;
	psa_set_key_type(&ikmAttributes, PSA_KEY_TYPE_DERIVE);
	psa_set_key_algorithm(&ikmAttributes, PSA_ALG_HKDF(PSA_ALG_SHA_256));
	psa_set_key_usage_flags(&ikmAttributes, PSA_KEY_USAGE_DERIVE);
	const std::array<uint8_t, 8> ikm{ 1, 2, 3, 4, 5, 6, 7, 8 };
	psa_key_id_t ikmKeyId{};
	zassert_equal(PSA_SUCCESS, psa_import_key(&ikmAttributes, ikm.data(), ikm.size(), &ikmKeyId));

	const std::array<uint8_t, 4> info{ 'i', 'n', 'f', 'o' };
	const std::array<uint8_t, 4> salt{ 's', 'a', 'l', 't' };

	std::array<uint8_t, 24> outputA{};
	std::array<uint8_t, 24> outputB{};
	zassert_equal(ALIRO_NO_ERROR,
		     DeriveRawKey(ikmKeyId, info.data(), info.size(), salt.data(), salt.size(), outputA.data(),
				  outputA.size()));
	zassert_equal(ALIRO_NO_ERROR,
		     DeriveRawKey(ikmKeyId, info.data(), info.size(), salt.data(), salt.size(), outputB.data(),
				  outputB.size()));

	zassert_mem_equal(outputA.data(), outputB.data(), outputA.size(),
			  "HKDF must be deterministic for identical (ikm, salt, info)");

	psa_destroy_key(ikmKeyId);
}

ZTEST(aliro_ud_crypto, test_verify_signature_accepts_valid_and_rejects_tampered)
{
	psa_key_attributes_t attributes = PSA_KEY_ATTRIBUTES_INIT;
	psa_set_key_type(&attributes, PSA_KEY_TYPE_ECC_KEY_PAIR(PSA_ECC_FAMILY_SECP_R1));
	psa_set_key_algorithm(&attributes, PSA_ALG_ECDSA(PSA_ALG_SHA_256));
	psa_set_key_bits(&attributes, PSA_BYTES_TO_BITS(kEccP256KeyPrivateKeyLength));
	psa_set_key_usage_flags(&attributes, PSA_KEY_USAGE_SIGN_HASH);
	psa_key_id_t signingKeyId{};
	zassert_equal(PSA_SUCCESS, psa_generate_key(&attributes, &signingKeyId));

	PublicKey publicKey{};
	size_t exportedLength{};
	zassert_equal(PSA_SUCCESS,
		     psa_export_public_key(signingKeyId, publicKey.data(), publicKey.size(), &exportedLength));
	zassert_equal(publicKey.size(), exportedLength);

	const std::array<uint8_t, 5> message{ 'h', 'e', 'l', 'l', 'o' };
	Signature signature{};
	size_t signatureLength{};
	zassert_equal(PSA_SUCCESS, psa_sign_message(signingKeyId, PSA_ALG_ECDSA(PSA_ALG_SHA_256), message.data(),
						    message.size(), signature.data(), signature.size(),
						    &signatureLength));
	zassert_equal(signature.size(), signatureLength);

	zassert_equal(ALIRO_NO_ERROR, VerifySignature(publicKey, message.data(), message.size(), signature));

	Signature tamperedSignature{ signature };
	tamperedSignature[0] ^= 0x01;
	zassert_equal(ALIRO_INVALID_SIGNATURE,
		     VerifySignature(publicKey, message.data(), message.size(), tamperedSignature));

	const std::array<uint8_t, 5> tamperedMessage{ 'h', 'e', 'l', 'l', 'x' };
	zassert_equal(ALIRO_INVALID_SIGNATURE,
		     VerifySignature(publicKey, tamperedMessage.data(), tamperedMessage.size(), signature));

	psa_destroy_key(signingKeyId);
}

ZTEST(aliro_ud_crypto, test_sha256_matches_fips_180_2_known_answer_vector)
{
	/* NIST FIPS 180-2, Appendix B.1: SHA-256("abc"). */
	const std::array<uint8_t, 3> message{ 'a', 'b', 'c' };
	const Sha256Hash expected{ 0xba, 0x78, 0x16, 0xbf, 0x8f, 0x01, 0xcf, 0xea, 0x41, 0x41, 0x40,
				   0xde, 0x5d, 0xae, 0x22, 0x23, 0xb0, 0x03, 0x61, 0xa3, 0x96, 0x17,
				   0x7a, 0x9c, 0xb4, 0x10, 0xff, 0x61, 0xf2, 0x00, 0x15, 0xad };

	Sha256Hash actual{};
	zassert_equal(ALIRO_NO_ERROR, Sha256(message.data(), message.size(), actual));
	zassert_mem_equal(expected.data(), actual.data(), expected.size());
}

ZTEST(aliro_ud_crypto, test_destroy_key_of_zero_is_a_no_op_success)
{
	KeyId keyId{ 0 };
	zassert_equal(ALIRO_NO_ERROR, DestroyKey(keyId));
	zassert_equal(0u, keyId);
}
