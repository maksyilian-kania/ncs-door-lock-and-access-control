/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include "test_reader.h"

#include <psa/crypto.h>

#include <algorithm>
#include <cstring>

namespace AliroUdTest::Reader {
namespace {

constexpr uint8_t kAliroCla{ 0x80 };
constexpr uint8_t kNfcInterfaceByte{ 0x5E };
constexpr std::array<uint8_t, 9> kExpeditedAid{ 0xA0, 0x00, 0x00, 0x09, 0x09, 0xAC, 0xCE, 0x55, 0x01 };
constexpr char kVolatileLabel[] = "Volatile****";

bool EnsurePsa()
{
	return psa_crypto_init() == PSA_SUCCESS;
}

void AppendTlv(Bytes &out, uint8_t tag, const uint8_t *value, size_t length)
{
	out.push_back(tag);
	if (length > 0x7F) {
		out.push_back(0x81);
	}
	out.push_back(static_cast<uint8_t>(length));
	out.insert(out.end(), value, value + length);
}

template <size_t N> void AppendTlv(Bytes &out, uint8_t tag, const std::array<uint8_t, N> &value)
{
	AppendTlv(out, tag, value.data(), value.size());
}

void AppendX(Bytes &out, const PublicKey &key)
{
	out.insert(out.end(), key.begin() + 1, key.begin() + 33);
}

Bytes CaseFourApdu(uint8_t cla, uint8_t ins, const Bytes &data)
{
	Bytes apdu{ cla, ins, 0x00, 0x00, static_cast<uint8_t>(data.size()) };
	apdu.insert(apdu.end(), data.begin(), data.end());
	apdu.push_back(0x00);
	return apdu;
}

/* Walks top-level TLVs; returns the offset of `tag` and its header/value lengths. */
bool LocateTlv(const Bytes &data, uint8_t tag, size_t &outOffset, size_t &outHeaderLength, size_t &outValueLength)
{
	size_t offset = 0;
	while (offset + 2 <= data.size()) {
		const uint8_t currentTag = data[offset];
		size_t headerLength = 2;
		size_t valueLength = data[offset + 1];
		if (valueLength == 0x81) {
			if (offset + 3 > data.size()) {
				return false;
			}
			valueLength = data[offset + 2];
			headerLength = 3;
		} else if (valueLength == 0x82) {
			if (offset + 4 > data.size()) {
				return false;
			}
			valueLength = (static_cast<size_t>(data[offset + 2]) << 8) | data[offset + 3];
			headerLength = 4;
		} else if (valueLength > 0x7F) {
			return false;
		}
		if (offset + headerLength + valueLength > data.size()) {
			return false;
		}
		if (currentTag == tag) {
			outOffset = offset;
			outHeaderLength = headerLength;
			outValueLength = valueLength;
			return true;
		}
		offset += headerLength + valueLength;
	}
	return false;
}

bool Hash(psa_algorithm_t algorithm, const Bytes &input, uint8_t *output, size_t outputSize)
{
	size_t length{};
	return EnsurePsa() &&
	       psa_hash_compute(algorithm, input.data(), input.size(), output, outputSize, &length) == PSA_SUCCESS &&
	       length == outputSize;
}

bool ImportKeyPair(const PrivateKey &privateKey, psa_algorithm_t algorithm, psa_key_usage_t usage,
		   psa_key_id_t &outKeyId)
{
	psa_key_attributes_t attributes = PSA_KEY_ATTRIBUTES_INIT;
	psa_set_key_type(&attributes, PSA_KEY_TYPE_ECC_KEY_PAIR(PSA_ECC_FAMILY_SECP_R1));
	psa_set_key_bits(&attributes, 256);
	psa_set_key_algorithm(&attributes, algorithm);
	psa_set_key_usage_flags(&attributes, usage);
	return EnsurePsa() && psa_import_key(&attributes, privateKey.data(), privateKey.size(), &outKeyId) ==
				      PSA_SUCCESS;
}

} // namespace

Response SplitResponse(const Bytes &rapdu)
{
	Response response{};
	if (rapdu.size() < 2) {
		return response;
	}
	response.mData.assign(rapdu.begin(), rapdu.end() - 2);
	response.mStatusWord = static_cast<uint16_t>((rapdu[rapdu.size() - 2] << 8) | rapdu[rapdu.size() - 1]);
	return response;
}

std::optional<Bytes> FindTlv(const Bytes &data, uint8_t tag)
{
	size_t offset{};
	size_t headerLength{};
	size_t valueLength{};
	if (!LocateTlv(data, tag, offset, headerLength, valueLength)) {
		return std::nullopt;
	}
	const auto begin = data.begin() + static_cast<std::ptrdiff_t>(offset + headerLength);
	return Bytes(begin, begin + static_cast<std::ptrdiff_t>(valueLength));
}

std::optional<Bytes> FindEncodedTlv(const Bytes &data, uint8_t tag)
{
	size_t offset{};
	size_t headerLength{};
	size_t valueLength{};
	if (!LocateTlv(data, tag, offset, headerLength, valueLength)) {
		return std::nullopt;
	}
	const auto begin = data.begin() + static_cast<std::ptrdiff_t>(offset);
	return Bytes(begin, begin + static_cast<std::ptrdiff_t>(headerLength + valueLength));
}

Bytes SelectCommand()
{
	Bytes apdu{ 0x00, 0xA4, 0x04, 0x00, static_cast<uint8_t>(kExpeditedAid.size()) };
	apdu.insert(apdu.end(), kExpeditedAid.begin(), kExpeditedAid.end());
	apdu.push_back(0x00);
	return apdu;
}

bool ParseSelectResponse(const Bytes &data, Bytes &outProprietaryInformationTlv)
{
	const auto fci = FindTlv(data, 0x6F);
	if (!fci.has_value()) {
		return false;
	}
	const auto proprietary = FindEncodedTlv(*fci, 0xA5);
	if (!proprietary.has_value()) {
		return false;
	}
	outProprietaryInformationTlv = *proprietary;
	return true;
}

bool BeginTransaction(const std::array<uint8_t, 16> &readerGroupIdentifier,
		      const std::array<uint8_t, 16> &readerGroupSubIdentifier, uint8_t authenticationPolicy,
		      const Bytes &proprietaryInformationTlv, Transaction &outTransaction)
{
	outTransaction = Transaction{};
	if (!EnsurePsa()) {
		return false;
	}

	std::copy(readerGroupIdentifier.begin(), readerGroupIdentifier.end(), outTransaction.mReaderIdentifier.begin());
	std::copy(readerGroupSubIdentifier.begin(), readerGroupSubIdentifier.end(),
		  outTransaction.mReaderIdentifier.begin() + readerGroupIdentifier.size());
	outTransaction.mAuthenticationPolicy = authenticationPolicy;
	outTransaction.mProprietaryInformationTlv = proprietaryInformationTlv;

	if (psa_generate_random(outTransaction.mTransactionIdentifier.data(),
				outTransaction.mTransactionIdentifier.size()) != PSA_SUCCESS) {
		return false;
	}

	psa_key_attributes_t attributes = PSA_KEY_ATTRIBUTES_INIT;
	psa_set_key_type(&attributes, PSA_KEY_TYPE_ECC_KEY_PAIR(PSA_ECC_FAMILY_SECP_R1));
	psa_set_key_bits(&attributes, 256);
	psa_set_key_algorithm(&attributes, PSA_ALG_ECDH);
	psa_set_key_usage_flags(&attributes, PSA_KEY_USAGE_DERIVE | PSA_KEY_USAGE_EXPORT);

	psa_key_id_t keyId{};
	if (psa_generate_key(&attributes, &keyId) != PSA_SUCCESS) {
		return false;
	}

	size_t privateLength{};
	size_t publicLength{};
	const bool exported =
		psa_export_key(keyId, outTransaction.mReaderEphemeralPrivateKey.data(),
			       outTransaction.mReaderEphemeralPrivateKey.size(), &privateLength) == PSA_SUCCESS &&
		psa_export_public_key(keyId, outTransaction.mReaderEphemeralPublicKey.data(),
				      outTransaction.mReaderEphemeralPublicKey.size(), &publicLength) == PSA_SUCCESS;
	psa_destroy_key(keyId);

	return exported && privateLength == outTransaction.mReaderEphemeralPrivateKey.size() &&
	       publicLength == outTransaction.mReaderEphemeralPublicKey.size();
}

Bytes Auth0Command(const Transaction &transaction)
{
	Bytes data{};
	AppendTlv(data, 0x41, &transaction.mCommandParameters, 1);
	AppendTlv(data, 0x42, &transaction.mAuthenticationPolicy, 1);
	AppendTlv(data, 0x5C, transaction.mProtocolVersion);
	AppendTlv(data, 0x87, transaction.mReaderEphemeralPublicKey);
	AppendTlv(data, 0x4C, transaction.mTransactionIdentifier);
	AppendTlv(data, 0x4D, transaction.mReaderIdentifier);
	return CaseFourApdu(kAliroCla, 0x80, data);
}

bool ParseAuth0Response(const Bytes &data, PublicKey &outCredentialEphemeralPublicKey)
{
	const auto key = FindTlv(data, 0x86);
	if (!key.has_value() || key->size() != outCredentialEphemeralPublicKey.size() || (*key)[0] != 0x04 ||
	    data.size() != 2 + key->size()) {
		return false;
	}
	std::copy(key->begin(), key->end(), outCredentialEphemeralPublicKey.begin());
	return true;
}

Bytes LoadCertCommand(const Bytes &compressedReaderCertificate)
{
	Bytes apdu{ kAliroCla, 0xD1, 0x00, 0x00, static_cast<uint8_t>(compressedReaderCertificate.size()) };
	apdu.insert(apdu.end(), compressedReaderCertificate.begin(), compressedReaderCertificate.end());
	return apdu;
}

Bytes Auth1Command(uint8_t commandParameters, const Signature &readerSignature,
		   const std::optional<Bytes> &readerCertificate)
{
	Bytes data{};
	AppendTlv(data, 0x41, &commandParameters, 1);
	AppendTlv(data, 0x9E, readerSignature);
	if (readerCertificate.has_value()) {
		AppendTlv(data, 0x90, readerCertificate->data(), readerCertificate->size());
	}
	return CaseFourApdu(kAliroCla, 0x81, data);
}

Bytes AuthenticationData(const Transaction &transaction, uint32_t usage)
{
	Bytes data{};
	AppendTlv(data, 0x4D, transaction.mReaderIdentifier);
	data.push_back(0x86);
	data.push_back(32);
	AppendX(data, transaction.mCredentialEphemeralPublicKey);
	data.push_back(0x87);
	data.push_back(32);
	AppendX(data, transaction.mReaderEphemeralPublicKey);
	AppendTlv(data, 0x4C, transaction.mTransactionIdentifier);
	const std::array<uint8_t, 4> usageBytes{ static_cast<uint8_t>(usage >> 24), static_cast<uint8_t>(usage >> 16),
						 static_cast<uint8_t>(usage >> 8), static_cast<uint8_t>(usage) };
	AppendTlv(data, 0x93, usageBytes);
	return data;
}

bool Sign(const PrivateKey &privateKey, const Bytes &message, Signature &outSignature)
{
	psa_key_id_t keyId{};
	if (!ImportKeyPair(privateKey, PSA_ALG_ECDSA(PSA_ALG_SHA_256), PSA_KEY_USAGE_SIGN_MESSAGE, keyId)) {
		return false;
	}
	size_t length{};
	const psa_status_t status = psa_sign_message(keyId, PSA_ALG_ECDSA(PSA_ALG_SHA_256), message.data(),
						     message.size(), outSignature.data(), outSignature.size(), &length);
	psa_destroy_key(keyId);
	return status == PSA_SUCCESS && length == outSignature.size();
}

bool Verify(const PublicKey &publicKey, const Bytes &message, const Signature &signature)
{
	psa_key_attributes_t attributes = PSA_KEY_ATTRIBUTES_INIT;
	psa_set_key_type(&attributes, PSA_KEY_TYPE_ECC_PUBLIC_KEY(PSA_ECC_FAMILY_SECP_R1));
	psa_set_key_bits(&attributes, 256);
	psa_set_key_algorithm(&attributes, PSA_ALG_ECDSA(PSA_ALG_SHA_256));
	psa_set_key_usage_flags(&attributes, PSA_KEY_USAGE_VERIFY_MESSAGE);

	psa_key_id_t keyId{};
	if (!EnsurePsa() || psa_import_key(&attributes, publicKey.data(), publicKey.size(), &keyId) != PSA_SUCCESS) {
		return false;
	}
	const psa_status_t status = psa_verify_message(keyId, PSA_ALG_ECDSA(PSA_ALG_SHA_256), message.data(),
						       message.size(), signature.data(), signature.size());
	psa_destroy_key(keyId);
	return status == PSA_SUCCESS;
}

bool PublicKeyFor(const PrivateKey &privateKey, PublicKey &outPublicKey)
{
	psa_key_id_t keyId{};
	if (!ImportKeyPair(privateKey, PSA_ALG_ECDSA(PSA_ALG_SHA_256), PSA_KEY_USAGE_SIGN_MESSAGE, keyId)) {
		return false;
	}
	size_t length{};
	const psa_status_t status = psa_export_public_key(keyId, outPublicKey.data(), outPublicKey.size(), &length);
	psa_destroy_key(keyId);
	return status == PSA_SUCCESS && length == outPublicKey.size();
}

bool DeriveSessionKeys(const Transaction &transaction, const PublicKey &readerGroupIdentifierKey,
		       SessionKeys &outKeys)
{
	outKeys = SessionKeys{};

	/* Section 8.3.1.4: Z_AB = ECKA-DH(reader_ePrivK, credential_ePubK). */
	psa_key_id_t keyId{};
	if (!ImportKeyPair(transaction.mReaderEphemeralPrivateKey, PSA_ALG_ECDH, PSA_KEY_USAGE_DERIVE, keyId)) {
		return false;
	}
	std::array<uint8_t, 32> sharedSecret{};
	size_t sharedLength{};
	const psa_status_t agreementStatus =
		psa_raw_key_agreement(PSA_ALG_ECDH, keyId, transaction.mCredentialEphemeralPublicKey.data(),
				      transaction.mCredentialEphemeralPublicKey.size(), sharedSecret.data(),
				      sharedSecret.size(), &sharedLength);
	psa_destroy_key(keyId);
	if (agreementStatus != PSA_SUCCESS || sharedLength != sharedSecret.size()) {
		return false;
	}

	/* X9.63 KDF, one SHA-256 block: Kdh = SHA-256(Z_AB || 00000001 || transaction_identifier). */
	Bytes kdfInput(sharedSecret.begin(), sharedSecret.end());
	kdfInput.insert(kdfInput.end(), { 0x00, 0x00, 0x00, 0x01 });
	kdfInput.insert(kdfInput.end(), transaction.mTransactionIdentifier.begin(),
			transaction.mTransactionIdentifier.end());
	if (!Hash(PSA_ALG_SHA_256, kdfInput, outKeys.mKdh.data(), outKeys.mKdh.size())) {
		return false;
	}

	/* Section 8.3.1.13 salt_volatile and info. */
	Bytes salt{};
	AppendX(salt, readerGroupIdentifierKey);
	salt.insert(salt.end(), kVolatileLabel, kVolatileLabel + std::strlen(kVolatileLabel));
	salt.insert(salt.end(), transaction.mReaderIdentifier.begin(), transaction.mReaderIdentifier.end());
	salt.push_back(kNfcInterfaceByte);
	salt.insert(salt.end(), { 0x5C, 0x02 });
	salt.insert(salt.end(), transaction.mProtocolVersion.begin(), transaction.mProtocolVersion.end());
	AppendX(salt, transaction.mReaderEphemeralPublicKey);
	salt.insert(salt.end(), transaction.mTransactionIdentifier.begin(), transaction.mTransactionIdentifier.end());
	salt.push_back(transaction.mCommandParameters);
	salt.push_back(transaction.mAuthenticationPolicy);
	salt.insert(salt.end(), transaction.mProprietaryInformationTlv.begin(),
		    transaction.mProprietaryInformationTlv.end());

	Bytes info{};
	AppendX(info, transaction.mCredentialEphemeralPublicKey);

	/* Section 8.3.1.5: HKDF-SHA-256; ExpeditedSKReader at offset 0, ExpeditedSKDevice at offset 32. */
	psa_key_derivation_operation_t operation = PSA_KEY_DERIVATION_OPERATION_INIT;
	std::array<uint8_t, 64> derived{};
	const bool derivedOk =
		psa_key_derivation_setup(&operation, PSA_ALG_HKDF(PSA_ALG_SHA_256)) == PSA_SUCCESS &&
		psa_key_derivation_input_bytes(&operation, PSA_KEY_DERIVATION_INPUT_SALT, salt.data(), salt.size()) ==
			PSA_SUCCESS &&
		psa_key_derivation_input_bytes(&operation, PSA_KEY_DERIVATION_INPUT_SECRET, outKeys.mKdh.data(),
					       outKeys.mKdh.size()) == PSA_SUCCESS &&
		psa_key_derivation_input_bytes(&operation, PSA_KEY_DERIVATION_INPUT_INFO, info.data(), info.size()) ==
			PSA_SUCCESS &&
		psa_key_derivation_output_bytes(&operation, derived.data(), derived.size()) == PSA_SUCCESS;
	psa_key_derivation_abort(&operation);
	if (!derivedOk) {
		return false;
	}

	std::copy_n(derived.begin(), 32, outKeys.mExpeditedSkReader.begin());
	std::copy_n(derived.begin() + 32, 32, outKeys.mExpeditedSkDevice.begin());
	return true;
}

bool DecryptDeviceResponse(const SymmetricKey &expeditedSkDevice, uint32_t deviceCounter, const Bytes &encrypted,
			   Bytes &outPlaintext)
{
	outPlaintext.clear();
	if (!EnsurePsa() || encrypted.size() < 16) {
		return false;
	}

	psa_key_attributes_t attributes = PSA_KEY_ATTRIBUTES_INIT;
	psa_set_key_type(&attributes, PSA_KEY_TYPE_AES);
	psa_set_key_bits(&attributes, 256);
	psa_set_key_algorithm(&attributes, PSA_ALG_GCM);
	psa_set_key_usage_flags(&attributes, PSA_KEY_USAGE_DECRYPT);

	psa_key_id_t keyId{};
	if (psa_import_key(&attributes, expeditedSkDevice.data(), expeditedSkDevice.size(), &keyId) != PSA_SUCCESS) {
		return false;
	}

	const std::array<uint8_t, 12> nonce{ 0x00,
					     0x00,
					     0x00,
					     0x00,
					     0x00,
					     0x00,
					     0x00,
					     0x01,
					     static_cast<uint8_t>(deviceCounter >> 24),
					     static_cast<uint8_t>(deviceCounter >> 16),
					     static_cast<uint8_t>(deviceCounter >> 8),
					     static_cast<uint8_t>(deviceCounter) };
	outPlaintext.resize(encrypted.size() - 16);
	size_t length{};
	const psa_status_t status = psa_aead_decrypt(keyId, PSA_ALG_GCM, nonce.data(), nonce.size(), nullptr, 0,
						     encrypted.data(), encrypted.size(), outPlaintext.data(),
						     outPlaintext.size(), &length);
	psa_destroy_key(keyId);
	if (status != PSA_SUCCESS || length != outPlaintext.size()) {
		outPlaintext.clear();
		return false;
	}
	return true;
}

bool KeySlot(const PublicKey &credentialPublicKey, std::array<uint8_t, 8> &outKeySlot)
{
	std::array<uint8_t, 20> digest{};
	if (!Hash(PSA_ALG_SHA_1, Bytes(credentialPublicKey.begin(), credentialPublicKey.end()), digest.data(),
		  digest.size())) {
		return false;
	}
	std::copy_n(digest.begin(), outKeySlot.size(), outKeySlot.begin());
	return true;
}

bool EncryptReaderCommand(const SymmetricKey &expeditedSkReader, uint32_t readerCounter, const Bytes &plaintext,
			  Bytes &outEncrypted)
{
	outEncrypted.clear();
	if (!EnsurePsa()) {
		return false;
	}

	psa_key_attributes_t attributes = PSA_KEY_ATTRIBUTES_INIT;
	psa_set_key_type(&attributes, PSA_KEY_TYPE_AES);
	psa_set_key_bits(&attributes, 256);
	psa_set_key_algorithm(&attributes, PSA_ALG_GCM);
	psa_set_key_usage_flags(&attributes, PSA_KEY_USAGE_ENCRYPT);

	psa_key_id_t keyId{};
	if (psa_import_key(&attributes, expeditedSkReader.data(), expeditedSkReader.size(), &keyId) != PSA_SUCCESS) {
		return false;
	}

	const std::array<uint8_t, 12> nonce{ 0x00,
					     0x00,
					     0x00,
					     0x00,
					     0x00,
					     0x00,
					     0x00,
					     0x00,
					     static_cast<uint8_t>(readerCounter >> 24),
					     static_cast<uint8_t>(readerCounter >> 16),
					     static_cast<uint8_t>(readerCounter >> 8),
					     static_cast<uint8_t>(readerCounter) };
	outEncrypted.resize(plaintext.size() + 16);
	size_t length{};
	const psa_status_t status = psa_aead_encrypt(keyId, PSA_ALG_GCM, nonce.data(), nonce.size(), nullptr, 0,
						     plaintext.data(), plaintext.size(), outEncrypted.data(),
						     outEncrypted.size(), &length);
	psa_destroy_key(keyId);
	if (status != PSA_SUCCESS || length != outEncrypted.size()) {
		outEncrypted.clear();
		return false;
	}
	return true;
}

Bytes ExchangeCommand(const Bytes &encrypted)
{
	return CaseFourApdu(kAliroCla, 0xC9, encrypted);
}

Bytes ControlFlowCommand(uint8_t s1Parameter, uint8_t s2Parameter)
{
	Bytes data{};
	AppendTlv(data, 0x41, &s1Parameter, 1);
	AppendTlv(data, 0x42, &s2Parameter, 1);
	Bytes apdu{ kAliroCla, 0x3C, 0x00, 0x00, static_cast<uint8_t>(data.size()) };
	apdu.insert(apdu.end(), data.begin(), data.end());
	return apdu;
}

namespace Exchange {

Bytes AtomicSession(bool start)
{
	const uint8_t option = start ? 0x01 : 0x00;
	Bytes out{};
	AppendTlv(out, 0x8C, &option, 1);
	return out;
}

Bytes ReadRequest(uint16_t offset, uint16_t length)
{
	const std::array<uint8_t, 4> value{ static_cast<uint8_t>(offset >> 8), static_cast<uint8_t>(offset),
					    static_cast<uint8_t>(length >> 8), static_cast<uint8_t>(length) };
	Bytes out{};
	AppendTlv(out, 0x87, value);
	return out;
}

Bytes WriteRequest(uint16_t offset, const Bytes &data)
{
	Bytes value{ static_cast<uint8_t>(offset >> 8), static_cast<uint8_t>(offset) };
	value.insert(value.end(), data.begin(), data.end());
	Bytes out{};
	AppendTlv(out, 0x8A, value.data(), value.size());
	return out;
}

Bytes SetRequest(uint16_t offset, uint16_t length, uint8_t value)
{
	const std::array<uint8_t, 5> fields{ static_cast<uint8_t>(offset >> 8), static_cast<uint8_t>(offset),
					     static_cast<uint8_t>(length >> 8), static_cast<uint8_t>(length), value };
	Bytes out{};
	AppendTlv(out, 0x95, fields);
	return out;
}

Bytes MailboxCommands(const Bytes &requests)
{
	Bytes out{};
	AppendTlv(out, 0xBA, requests.data(), requests.size());
	return out;
}

Bytes ReaderStatus(uint8_t firstByte, uint8_t secondByte)
{
	const std::array<uint8_t, 2> value{ firstByte, secondByte };
	Bytes out{};
	AppendTlv(out, 0x97, value);
	return out;
}

} // namespace Exchange

bool ParseExchangeResponse(const Bytes &plaintext, ExchangeResponse &outResponse)
{
	outResponse = ExchangeResponse{};
	std::vector<Bytes> blocks{};
	size_t offset = 0;
	while (offset < plaintext.size()) {
		if (offset + 2 > plaintext.size()) {
			return false;
		}
		const size_t length = (static_cast<size_t>(plaintext[offset]) << 8) | plaintext[offset + 1];
		offset += 2;
		if (offset + length > plaintext.size()) {
			return false;
		}
		const auto begin = plaintext.begin() + static_cast<std::ptrdiff_t>(offset);
		blocks.emplace_back(begin, begin + static_cast<std::ptrdiff_t>(length));
		offset += length;
	}

	if (blocks.empty() || blocks.back().size() != 2) {
		return false;
	}
	outResponse.mStatus = static_cast<uint16_t>((blocks.back()[0] << 8) | blocks.back()[1]);
	blocks.pop_back();
	outResponse.mReadData = std::move(blocks);
	return true;
}

} // namespace AliroUdTest::Reader
