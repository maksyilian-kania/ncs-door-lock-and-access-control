/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include <zephyr/ztest.h>

#include <aliro/user_device/interface.h>

#include <psa/crypto.h>

#include "user_device/crypto/access_protocol_crypto.h"

#include <array>
#include <cstring>
#include <string_view>
#include <vector>

/*
 * Aliro::Interface::UserDevice::Crypto PSA Crypto bindings under test
 * (APP_PLAN.md AWP5, platform/crypto/crypto.cpp).
 *
 * SHA-256 uses the NIST FIPS 180-2 "abc" vector. HKDF, SHA-1/key_slot, and
 * User-Device-direction AES-GCM use immutable Aliro Appendix 14.3 values;
 * Reader-direction AES-GCM uses the Appendix key with an independently
 * generated cryptography-41.0.7 reference value. ECDH and ECDSA retain their
 * self-consistent positive/tamper coverage.
 */

using namespace Aliro;
using namespace Aliro::CryptoTypes;
using namespace Aliro::Interface::UserDevice::Crypto;

namespace {

void ResetGlobalState(void *fixture)
{
	(void)fixture;
}

uint8_t HexNibble(char value)
{
	return value <= '9' ? static_cast<uint8_t>(value - '0') : static_cast<uint8_t>((value | 0x20) - 'a' + 10);
}

std::vector<uint8_t> Hex(std::string_view encoded)
{
	std::vector<uint8_t> result{};
	result.reserve(encoded.size() / 2);
	for (size_t i = 0; i < encoded.size(); i += 2) {
		result.push_back(static_cast<uint8_t>((HexNibble(encoded[i]) << 4) | HexNibble(encoded[i + 1])));
	}
	return result;
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
	zassert_equal(PSA_SUCCESS, psa_import_key(&ikmAttributes, secret.data(), secret.size(), &ikmKeyId));

	const std::array<uint8_t, 4> info{ 'i', 'n', 'f', 'o' };
	const std::array<uint8_t, 4> salt{ 's', 'a', 'l', 't' };

	KeyId symmetricKeyId{};
	zassert_equal(ALIRO_NO_ERROR,
		      DeriveSymmetricKey(ikmKeyId, info.data(), info.size(), salt.data(), salt.size(), symmetricKeyId));
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
	zassert_equal(ALIRO_NO_ERROR, DeriveRawKey(ikmKeyId, info.data(), info.size(), salt.data(), salt.size(),
						   outputA.data(), outputA.size()));
	zassert_equal(ALIRO_NO_ERROR, DeriveRawKey(ikmKeyId, info.data(), info.size(), salt.data(), salt.size(),
						   outputB.data(), outputB.size()));

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
	zassert_equal(PSA_SUCCESS,
		      psa_sign_message(signingKeyId, PSA_ALG_ECDSA(PSA_ALG_SHA_256), message.data(), message.size(),
				       signature.data(), signature.size(), &signatureLength));
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

ZTEST(aliro_ud_crypto, test_sha1_and_key_slot_match_known_answer)
{
	/*
	 * Access Credential long-term public key from Aliro 1.0 Appendix 14.3,
	 * pp. 176-177. SHA-1 was independently checked with Python hashlib;
	 * section 8.3.3.4.2, p. 76 takes the first eight bytes.
	 */
	const auto publicKeyBytes = Hex("0488f6f8f2f1e35a58879e72d9ea81957e8964c3d3c566eb9d41c83d0d8c63"
					"a23075dbdcf67d15bda429db38706a2f15ba90a2ac3c6a00973d21ed758c1471"
					"a748");
	PublicKey publicKey{};
	zassert_equal(publicKey.size(), publicKeyBytes.size());
	memcpy(publicKey.data(), publicKeyBytes.data(), publicKey.size());

	const auto expectedHash = Hex("e791c72dea5ec7d757bd3caff8e1f7ffa016390f");
	Sha1Hash actualHash{};
	zassert_equal(ALIRO_NO_ERROR, Sha1(publicKey.data(), publicKey.size(), actualHash));
	zassert_mem_equal(expectedHash.data(), actualHash.data(), actualHash.size());

	const std::array<uint8_t, UserDevice::AccessProtocol::kKeySlotLength> expectedSlot{ 0xE7, 0x91, 0xC7, 0x2D,
											    0xEA, 0x5E, 0xC7, 0xD7 };
	std::array<uint8_t, UserDevice::AccessProtocol::kKeySlotLength> actualSlot{};
	zassert_equal(ALIRO_NO_ERROR, UserDevice::Crypto::DeriveKeySlot(publicKey, actualSlot));
	zassert_mem_equal(expectedSlot.data(), actualSlot.data(), actualSlot.size());
}

ZTEST(aliro_ud_crypto, test_appendix_14_3_hkdf_and_output_slices)
{
	/* Aliro 1.0 Specification Appendix 14.3, pp. 176-177. */
	const auto kdh = Hex("cd227f01f917ad1dd5252db51c5ad3da1c3028be750a0f4e69c6a5624fca271c");
	const auto salt = Hex("b62d9b8f494f2f43a07a7db7e965865d04feeabe4e9c3b8a2f5a544ee2a9c60f"
			      "566f6c6174696c652a2a2a2a"
			      "00112233445566778899aabbccddeeffffeeddccbbaa99887766554433221100"
			      "5e5c020100"
			      "9696afe33de58b7d3253d1cba86d14147c16d455e8a27373b38d454af21b70e7"
			      "4165a83667ad0af5ab115247424822e0"
			      "0001"
			      "a508800200005c020100");
	const auto info = Hex("5d75ab60136a2c54ff27b799ee157f3f3329435c0df608de904c920ac29f72bd");
	const auto expected = Hex("f06ab1499102ca96f75cfa6d2e42c7920382d05a22e959325a91eb3aa4d71ce8"
				  "de82f4f94575da8369febd52dea94ec3dadad6d4406a9efe76098d6a22a8fd5d"
				  "b3cdefdb7dae91722efea57ee5f0981b1b3d5e436b406376635bcfd85b562bee"
				  "8f770f08d0fedea9c441f5f40b1bff1aaad92547729853ceb23a965761d8799f"
				  "9143579775f7b7463e527c9b8f0a581f31ecadff8c82517372666d0bc7a426db");

	KeyId kdhKey{};
	zassert_equal(ALIRO_NO_ERROR, ImportKey(kdh.data(), kdh.size(), kdhKey));
	std::array<uint8_t, 160> derived{};
	zassert_equal(ALIRO_NO_ERROR, DeriveRawKey(kdhKey, info.data(), info.size(), salt.data(), salt.size(),
						   derived.data(), derived.size()));
	zassert_equal(derived.size(), expected.size());
	zassert_mem_equal(expected.data(), derived.data(), derived.size(),
			  "PSA HKDF or normative output slicing differs from Appendix 14.3");
	zassert_equal(ALIRO_NO_ERROR, DestroyKey(kdhKey));
}

ZTEST(aliro_ud_crypto, test_appendix_and_reference_directional_aes_gcm_vectors)
{
	/* User Device response direction: exact Appendix 14.3 AUTH1 wrapped response. */
	const auto deviceKey = Hex("de82f4f94575da8369febd52dea94ec3dadad6d4406a9efe76098d6a22a8fd5d");
	const auto devicePlaintext = Hex("5a410488f6f8f2f1e35a58879e72d9ea81957e8964c3d3c566eb9d41c83d0d8c"
					 "63a23075dbdcf67d15bda429db38706a2f15ba90a2ac3c6a00973d21ed758c1471"
					 "a7489e402f57a5cb8a88c5a300fadb858d17298ed6f9dc01f9abc65e4b40894398"
					 "68b8d24e93f1e54ca1df0703a76974a847ebafb42a7e90dccc3aaed788251d155a"
					 "63e05e02003f");
	const auto expectedDeviceWrapped = Hex("caae4715cb099959b6354df09a754bdeb31689e27be440d0c2cfe8d4e5b5d99b"
					       "a367801c0f4f46485a160840f4e51b42d5b5e420157d64188af6d89921ce5fa4"
					       "82f7e51725ba7568e5976cf6e9443fa57b32fd76a6a1b1b3190bd2aa0ee946f4"
					       "8c65dc8f3dc24c652fb9cab1381a68f0737a77c5e2b1cfbd9884041049d3e37b"
					       "7126a2d74d7af03a322fbac65d627ef576a8d83e1a887b5be7");
	const Nonce deviceNonce{ 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01 };

	KeyId deviceKeyId{};
	zassert_equal(ALIRO_NO_ERROR, ImportKey(deviceKey.data(), deviceKey.size(), deviceKeyId));
	std::vector<uint8_t> deviceCiphertext(devicePlaintext.size());
	AuthenticationTag deviceTag{};
	zassert_equal(ALIRO_NO_ERROR, AeadEncrypt(deviceKeyId, devicePlaintext.data(), devicePlaintext.size(), nullptr,
						  0, deviceNonce, deviceCiphertext.data(), deviceTag));
	deviceCiphertext.insert(deviceCiphertext.end(), deviceTag.begin(), deviceTag.end());
	zassert_equal(expectedDeviceWrapped.size(), deviceCiphertext.size());
	zassert_mem_equal(expectedDeviceWrapped.data(), deviceCiphertext.data(), expectedDeviceWrapped.size());
	zassert_equal(ALIRO_NO_ERROR, DestroyKey(deviceKeyId));

	/*
	 * Reader command direction uses Appendix ExpeditedSKReader and the
	 * mandated 0x00...00 || reader_counter IV. Expected bytes were generated
	 * independently with Python cryptography 41.0.7 AESGCM.
	 */
	const auto readerKey = Hex("f06ab1499102ca96f75cfa6d2e42c7920382d05a22e959325a91eb3aa4d71ce8");
	const auto readerPlaintext = Hex("97020100");
	const auto expectedReaderWrapped = Hex("74ab7f73945d29896ada5d90b41711b18ceeb382");
	const Nonce readerNonce{ 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01 };
	KeyId readerKeyId{};
	zassert_equal(ALIRO_NO_ERROR, ImportKey(readerKey.data(), readerKey.size(), readerKeyId));
	std::array<uint8_t, 4> readerCiphertext{};
	AuthenticationTag readerTag{};
	zassert_equal(ALIRO_NO_ERROR, AeadEncrypt(readerKeyId, readerPlaintext.data(), readerPlaintext.size(), nullptr,
						  0, readerNonce, readerCiphertext.data(), readerTag));
	std::array<uint8_t, 20> readerWrapped{};
	memcpy(readerWrapped.data(), readerCiphertext.data(), readerCiphertext.size());
	memcpy(readerWrapped.data() + readerCiphertext.size(), readerTag.data(), readerTag.size());
	zassert_mem_equal(expectedReaderWrapped.data(), readerWrapped.data(), readerWrapped.size());
	zassert_equal(ALIRO_NO_ERROR, DestroyKey(readerKeyId));
}

ZTEST(aliro_ud_crypto, test_destroy_key_of_zero_is_a_no_op_success)
{
	KeyId keyId{ 0 };
	zassert_equal(ALIRO_NO_ERROR, DestroyKey(keyId));
	zassert_equal(0u, keyId);
}

ZTEST(aliro_ud_crypto, test_raw_key_agreement_rejects_off_curve_and_malformed_peer_public_key)
{
	KeyId localKeyId{};
	PublicKey localPublicKey{};
	zassert_equal(ALIRO_NO_ERROR, GenerateEphemeralKeyPair(localKeyId, localPublicKey));

	PublicKey offCurvePeer{};
	offCurvePeer[0] = kEccP256PublicKeyPrefix;
	offCurvePeer[1] = 0x01;
	offCurvePeer[2] = 0x01;
	SharedSecret secret{};
	zassert_equal(ALIRO_ERROR_INTERNAL, RawKeyAgreement(localKeyId, offCurvePeer, secret));

	PublicKey malformedPeer{};
	malformedPeer[0] = 0x03;
	malformedPeer.fill(0xAA);
	zassert_equal(ALIRO_ERROR_INTERNAL, RawKeyAgreement(localKeyId, malformedPeer, secret));

	PublicKey zeroCoordinatePeer{};
	zeroCoordinatePeer[0] = kEccP256PublicKeyPrefix;
	zassert_equal(ALIRO_ERROR_INTERNAL, RawKeyAgreement(localKeyId, zeroCoordinatePeer, secret));

	zassert_equal(ALIRO_NO_ERROR, DestroyKey(localKeyId));
}

ZTEST(aliro_ud_crypto, test_appendix_14_3_reader_signature_verifies_authentication_bytes)
{
	/* Aliro 1.0 Specification Appendix 14.3, pp. 176-177: Table 8-12 bytes and 64-byte r||s signature. */
	const auto authenticationBytes = Hex("4d2000112233445566778899aabbccddeeffffeeddccbbaa99887766554433221100"
					     "86205d75ab60136a2c54ff27b799ee157f3f3329435c0df608de904c920ac29f72bd"
					     "87209696afe33de58b7d3253d1cba86d14147c16d455e8a27373b38d454af21b70e7"
					     "4c104165a83667ad0af5ab115247424822e09304415d9569");
	const auto readerPublicKey = Hex("04b62d9b8f494f2f43a07a7db7e965865d04feeabe4e9c3b8a2f5a544ee2a9c60f"
					 "d8675c7b3cca0e0070dbb999d9d11f67b4517247452ec931eef51f047194172a");
	const auto readerSignatureBytes = Hex("501952e25339019804a7c3a7e4a1f6d993aec8baba7db6c8c20ac450428c2ff3"
					      "90c2188854ef7964927f88040dddf895ef57cce72379ad9688f36c5c7de3c294");

	PublicKey readerPublicKeyArray{};
	zassert_equal(readerPublicKey.size(), readerPublicKeyArray.size());
	memcpy(readerPublicKeyArray.data(), readerPublicKey.data(), readerPublicKey.size());

	Signature readerSignature{};
	zassert_equal(readerSignature.size(), readerSignatureBytes.size());
	memcpy(readerSignature.data(), readerSignatureBytes.data(), readerSignature.size());

	zassert_equal(ALIRO_NO_ERROR, VerifySignature(readerPublicKeyArray, authenticationBytes.data(),
						      authenticationBytes.size(), readerSignature));

	Signature tamperedSignature{ readerSignature };
	tamperedSignature[0] ^= 0x01;
	zassert_equal(ALIRO_INVALID_SIGNATURE, VerifySignature(readerPublicKeyArray, authenticationBytes.data(),
							       authenticationBytes.size(), tamperedSignature));

	PublicKey wrongKey{ readerPublicKeyArray };
	wrongKey.back() ^= 0x01;
	zassert_equal(ALIRO_INVALID_SIGNATURE, VerifySignature(wrongKey, authenticationBytes.data(),
							       authenticationBytes.size(), readerSignature));
}
