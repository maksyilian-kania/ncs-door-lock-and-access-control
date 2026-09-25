/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

/**
 * @brief Test-only Aliro Reader for host integration tests.
 *
 * Plays the Reader side of the Expedited Standard phase (Aliro 1.0
 * Specification and Test Plan, 26-42802-001, sections 8.3.1 and 8.3.3,
 * pp. 55-79) against the real `Aliro::UserDeviceStack`, using PSA Crypto
 * directly and never the application's `Aliro::Interface::UserDevice`
 * adapters, so an adapter defect cannot cancel itself out. It is a
 * verification instrument only: it has no User Device behavior.
 */
namespace AliroUdTest::Reader {

using Bytes = std::vector<uint8_t>;
using PublicKey = std::array<uint8_t, 65>;
using PrivateKey = std::array<uint8_t, 32>;
using Signature = std::array<uint8_t, 64>;
using SymmetricKey = std::array<uint8_t, 32>;
using ReaderIdentifier = std::array<uint8_t, 32>;
using TransactionIdentifier = std::array<uint8_t, 16>;

/** @brief AUTH1 `usage` values (Tables 8-12 and 8-13, pp. 76-77). */
constexpr uint32_t kReaderSignatureUsage{ 0x415D9569 };
constexpr uint32_t kUserDeviceSignatureUsage{ 0x4E887B4C };

/** @brief An R-APDU split into its data field and status word. */
struct Response {
	Bytes mData;
	uint16_t mStatusWord{ 0 };
};

/** @brief All Reader-side, session-bound values of one Expedited Standard transaction. */
struct Transaction {
	ReaderIdentifier mReaderIdentifier{};
	TransactionIdentifier mTransactionIdentifier{};
	uint8_t mCommandParameters{ 0x00 };
	uint8_t mAuthenticationPolicy{ 0x01 };
	std::array<uint8_t, 2> mProtocolVersion{ 0x01, 0x00 };
	PrivateKey mReaderEphemeralPrivateKey{};
	PublicKey mReaderEphemeralPublicKey{};
	/** @brief The complete `0xA5` proprietary information TLV from the SELECT response (Table 10-2). */
	Bytes mProprietaryInformationTlv;
	PublicKey mCredentialEphemeralPublicKey{};
};

/** @brief Output of the Expedited Standard key material generation (sections 8.3.1.4 and 8.3.1.13). */
struct SessionKeys {
	SymmetricKey mKdh{};
	SymmetricKey mExpeditedSkReader{};
	SymmetricKey mExpeditedSkDevice{};
};

/** @brief Splits a raw R-APDU; an APDU shorter than 2 bytes yields status word 0. */
Response SplitResponse(const Bytes &rapdu);

/**
 * @brief Returns the value of the first top-level TLV with single-byte `tag`
 * in `data` (1-byte, `0x81`, or `0x82` lengths), or `std::nullopt` if absent
 * or malformed.
 */
std::optional<Bytes> FindTlv(const Bytes &data, uint8_t tag);

/** @brief Returns the complete (tag, length, value) encoding of the first top-level TLV with `tag`. */
std::optional<Bytes> FindEncodedTlv(const Bytes &data, uint8_t tag);

/** @brief SELECT of the expedited-phase AID `A000000909ACCE5501` (section 10.2.1.2). */
Bytes SelectCommand();

/** @brief Extracts the `0xA5` proprietary information TLV from a SELECT FCI response data field. */
bool ParseSelectResponse(const Bytes &data, Bytes &outProprietaryInformationTlv);

/**
 * @brief Starts a fresh Expedited Standard transaction: new random reader
 * ephemeral key pair and transaction identifier (section 8.3.3.2.5, p. 68).
 */
bool BeginTransaction(const std::array<uint8_t, 16> &readerGroupIdentifier,
		      const std::array<uint8_t, 16> &readerGroupSubIdentifier, uint8_t authenticationPolicy,
		      const Bytes &proprietaryInformationTlv, Transaction &outTransaction);

/** @brief AUTH0 command APDU (Tables 8-3 and 8-4, pp. 65-66). */
Bytes Auth0Command(const Transaction &transaction);

/**
 * @brief Parses an Expedited Standard AUTH0 response data field (Table 8-5,
 * p. 67): exactly one `0x86` TLV carrying a 65-byte uncompressed point.
 */
bool ParseAuth0Response(const Bytes &data, PublicKey &outCredentialEphemeralPublicKey);

/** @brief LOAD CERT command APDU carrying a compressed `reader_Cert` (Table 8-8, p. 71). */
Bytes LoadCertCommand(const Bytes &compressedReaderCertificate);

/** @brief AUTH1 command APDU (Tables 8-9 and 8-10, pp. 72-73); `readerCertificate` becomes tag `0x90`. */
Bytes Auth1Command(uint8_t commandParameters, const Signature &readerSignature,
		   const std::optional<Bytes> &readerCertificate = std::nullopt);

/** @brief AUTH1 authentication data fields for `usage` (Tables 8-12 and 8-13, pp. 76-77). */
Bytes AuthenticationData(const Transaction &transaction, uint32_t usage);

/** @brief ECDSA P-256/SHA-256 signature over `message` (section 8.3.1.2, p. 55). */
bool Sign(const PrivateKey &privateKey, const Bytes &message, Signature &outSignature);

/** @brief ECDSA P-256/SHA-256 verification of `signature` over `message`. */
bool Verify(const PublicKey &publicKey, const Bytes &message, const Signature &signature);

/** @brief Derives the P-256 public key for `privateKey`. */
bool PublicKeyFor(const PrivateKey &privateKey, PublicKey &outPublicKey);

/**
 * @brief Reader-side Kdh and volatile session keys (sections 8.3.1.4,
 * 8.3.1.5, and 8.3.1.13, pp. 55-59) for NFC interface byte `0x5E`.
 *
 * `readerGroupIdentifierKey` is the key bound to the reader_group_identifier:
 * the Reader public key without a certificate, or the Reader System Issuer
 * CA public key with one (section 6.2, p. 28).
 */
bool DeriveSessionKeys(const Transaction &transaction, const PublicKey &readerGroupIdentifierKey,
		       SessionKeys &outKeys);

/**
 * @brief Verifies and decrypts a secure-channel response with
 * `IV = 0x0000000000000001 || device_counter` (sections 8.3.1.6-7, p. 56).
 */
bool DecryptDeviceResponse(const SymmetricKey &expeditedSkDevice, uint32_t deviceCounter, const Bytes &encrypted,
			   Bytes &outPlaintext);

/** @brief First 8 bytes of SHA-1 over the uncompressed Access Credential public key (section 8.3.3.4.2, p. 76). */
bool KeySlot(const PublicKey &credentialPublicKey, std::array<uint8_t, 8> &outKeySlot);

/**
 * @brief Encrypts a secure-channel command payload with
 * `IV = 0x0000000000000000 || reader_counter` (section 8.3.1.8, p. 56).
 * Returns `encrypted_payload || authentication_tag`.
 */
bool EncryptReaderCommand(const SymmetricKey &expeditedSkReader, uint32_t readerCounter, const Bytes &plaintext,
			  Bytes &outEncrypted);

/** @brief EXCHANGE command APDU (Table 8-14, p. 81) carrying `encrypted_payload || authentication_tag`. */
Bytes ExchangeCommand(const Bytes &encrypted);

/** @brief CONTROL FLOW command APDU (Tables 10-5 and 10-6, p. 97). */
Bytes ControlFlowCommand(uint8_t s1Parameter, uint8_t s2Parameter);

/** @brief EXCHANGE command payload elements before encryption (Tables 8-15 and 8-16, pp. 81-82). */
namespace Exchange {

/** @brief `0x8C` mailbox option byte: bit0 starts (1) or stops (0) an atomic session. */
Bytes AtomicSession(bool start);

/** @brief `0x87` read request. */
Bytes ReadRequest(uint16_t offset, uint16_t length);

/** @brief `0x8A` write request. */
Bytes WriteRequest(uint16_t offset, const Bytes &data);

/** @brief `0x95` set request: fill `length` bytes at `offset` with `value`. */
Bytes SetRequest(uint16_t offset, uint16_t length, uint8_t value);

/** @brief `0xBA` mailbox commands wrapping the concatenated `requests`. */
Bytes MailboxCommands(const Bytes &requests);

/** @brief `0x97` Reader status (Table 8-18, p. 84). */
Bytes ReaderStatus(uint8_t firstByte, uint8_t secondByte);

} // namespace Exchange

/**
 * @brief A decrypted EXCHANGE response payload (Table 8-20, p. 86, and
 * section 8.3.3.5.5, p. 87): big-endian 2-byte length-prefixed blocks, the
 * last being the status `0x0002 || B1 || B2`.
 */
struct ExchangeResponse {
	std::vector<Bytes> mReadData;
	uint16_t mStatus{ 0xFFFF };
};

/** @brief Splits a decrypted EXCHANGE response payload; false if it is not a well-formed block sequence ending in a status block. */
bool ParseExchangeResponse(const Bytes &plaintext, ExchangeResponse &outResponse);

} // namespace AliroUdTest::Reader
