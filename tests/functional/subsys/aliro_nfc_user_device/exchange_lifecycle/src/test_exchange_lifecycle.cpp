/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include <zephyr/kernel.h>
#include <zephyr/ztest.h>

#include "fake_authorization_indicator.h"
#include "fake_credential_persistence.h"
#include "fake_key_backend.h"
#include "fake_mailbox_persistence.h"
#include "fake_nfc_interface.h"
#include "fake_persistent_key_backend.h"
#include "fake_persistent_key_persistence.h"
#include "log_capture.h"
#include "platform/authorization/authorization_window.h"
#include "platform/nfc/nfc_worker.h"
#include "storage/credential/credential_store.h"
#include "storage/key/persistent_key_store.h"
#include "storage/mailbox/mailbox_sessions.h"
#include "storage/mailbox/mailbox_store.h"
#include "test_reader.h"

#include <aliro/user_device/interface.h>

#include <array>
#include <cstring>
#include <string>

/*
 * Expedited Standard transactions continued through EXCHANGE (Aliro 1.0
 * Specification and Test Plan, 26-42802-001, section 8.3.3.5, pp. 80-88),
 * CONTROL FLOW (section 10.2.2.1, p. 97), field loss, and session timeout,
 * driven by the test Reader (common/test_reader.h) through the real worker
 * thread and the real Aliro::UserDeviceStack. The assertions target the
 * application-owned boundaries those phases reach: mailbox persistence and
 * snapshot sessions, the OS timer, transport termination, and transaction
 * notification. EXCHANGE parsing, response construction, status words, and
 * failure sequencing are the stack's; implementation-specific values
 * (B1/B2, error status words) are therefore not pinned.
 */

using namespace AliroUdTest;
using AliroUdTest::Reader::Bytes;

namespace {

constexpr uint16_t kSwSuccess{ 0x9000 };
constexpr uint16_t kExchangeSuccess{ 0x0000 };
constexpr size_t kMailboxSize{ 32 };
constexpr int kSessionTimeoutMs{ CONFIG_NCS_ALIRO_USER_DEVICE_SESSION_TIMEOUT_NFC };

Bytes Hex(const char *text)
{
	Bytes out{};
	const size_t length = std::strlen(text);
	for (size_t i = 0; i + 1 < length; i += 2) {
		out.push_back(static_cast<uint8_t>(std::stoul(std::string(text + i, 2), nullptr, 16)));
	}
	return out;
}

template <size_t N> std::array<uint8_t, N> HexArray(const char *text)
{
	const Bytes bytes = Hex(text);
	zassert_equal(N, bytes.size(), "Test vector has the wrong length");
	std::array<uint8_t, N> out{};
	std::copy(bytes.begin(), bytes.end(), out.begin());
	return out;
}

/*
 * Aliro 1.0 Specification and Test Plan, 26-42802-001, section 14.3
 * "Expedited-standard phase without Reader Certificate", pp. 175-177.
 */
namespace Spec143 {
constexpr char kReaderPrivateKey[] = "7a9e50a19ae385e39b3bf0c75eb5f9c9a5eb4d51f808231835395fd2c1078367";
constexpr char kReaderPublicKey[] = "04b62d9b8f494f2f43a07a7db7e965865d04feeabe4e9c3b8a2f5a544ee2a9c60fd8675c7b3cca0e0070dbb999d9d"
				    "11f67b4517247452ec931eef51f047194172a";
constexpr char kCredentialPrivateKey[] = "5b0e4716bfb90700984963a32e3ce1721f1bd39b5b8bd952b6423a78fdeedec1";
} // namespace Spec143

/* Dispatch settle time; the worker drains one event per wake. */
void SettleWorker()
{
	k_msleep(50);
}

std::array<uint8_t, 16> Filled16(uint8_t value)
{
	std::array<uint8_t, 16> out{};
	out.fill(value);
	return out;
}

/* Sends one C-APDU through the real worker and returns the single R-APDU it produced. */
Reader::Response Transceive(const Bytes &capdu)
{
	const size_t sentBefore = FakeNfc::GetSentResponseCount();
	zassert_equal(0, AliroUd::Nfc::PostCommandApdu(capdu.data(), capdu.size()), "Command must be queued");
	for (int i = 0; i < 100 && FakeNfc::GetSentResponseCount() == sentBefore; ++i) {
		k_msleep(10);
	}
	zassert_equal(sentBefore + 1, FakeNfc::GetSentResponseCount(), "Expected exactly one response APDU");
	return Reader::SplitResponse(FakeNfc::GetLastResponse());
}

struct MailboxRights {
	bool mReadable{ true };
	bool mWritable{ true };
};

constexpr MailboxRights kReadOnly{ true, false };
constexpr MailboxRights kWriteOnly{ false, true };

/* The §14.3 Access Credential bound, without a certificate, to the §14.3 Reader key, with a mailbox. */
::Aliro::UserDevice::CredentialHandle ProvisionWithMailbox(uint8_t groupFill, MailboxRights rights)
{
	AliroUd::Credential::Provisioning::Payload payload{};
	payload.mHasNewKeyInput = true;
	payload.mNewKeyScalar = HexArray<32>(Spec143::kCredentialPrivateKey);
	payload.mPolicySet = true;
	payload.mPolicy = ::Aliro::UserDevice::AuthenticationPolicy::UserDeviceSetting;
	payload.mBindingCount = 1;
	const auto group = Filled16(groupFill);
	std::copy(group.begin(), group.end(), payload.mBindings[0].mReaderGroupIdentifier.begin());
	payload.mBindings[0].mTrustType = AliroUd::Credential::TrustType::Direct;
	const auto readerKey = HexArray<65>(Spec143::kReaderPublicKey);
	std::copy(readerKey.begin(), readerKey.end(), payload.mBindings[0].mKey.begin());
	payload.mMailbox.mConfigured = true;
	payload.mMailbox.mSizeBytes = kMailboxSize;
	payload.mMailbox.mReadable = rights.mReadable;
	payload.mMailbox.mWritable = rights.mWritable;

	::Aliro::UserDevice::CredentialHandle handle{ ::Aliro::UserDevice::kInvalidCredentialHandle };
	zassert_equal(ALIRO_NO_ERROR, AliroUd::Credential::Store::Create(payload, handle),
		      "Provisioning the credential must succeed");
	return handle;
}

Bytes SeedPattern()
{
	Bytes out(kMailboxSize);
	for (size_t i = 0; i < out.size(); ++i) {
		out[i] = static_cast<uint8_t>(0xA0 + i);
	}
	return out;
}

/* Commits `content` as the mailbox's persistent pre-transaction data. */
void SeedMailbox(::Aliro::UserDevice::CredentialHandle handle, const Bytes &content)
{
	zassert_equal(ALIRO_NO_ERROR, AliroUd::Mailbox::Store::Initialize(handle));
	std::array<uint8_t, AliroUd::Mailbox::kMaxSizeBytes> data{};
	std::array<bool, AliroUd::Mailbox::kMaxSizeBytes> dirty{};
	std::copy(content.begin(), content.end(), data.begin());
	std::fill_n(dirty.begin(), content.size(), true);
	zassert_equal(ALIRO_NO_ERROR, AliroUd::Mailbox::Store::ApplyDirtyBytes(handle, data, dirty));
}

Bytes CommittedMailbox(::Aliro::UserDevice::CredentialHandle handle)
{
	Bytes out(kMailboxSize);
	zassert_equal(ALIRO_NO_ERROR, AliroUd::Mailbox::Store::RawRead(handle, 0, out.data(), out.size()));
	return out;
}

void AssertBytesEqual(const Bytes &expected, const Bytes &actual, const char *message)
{
	zassert_equal(expected.size(), actual.size(), "%s (length)", message);
	zassert_mem_equal(expected.data(), actual.data(), expected.size(), "%s", message);
}

/* Reader-side secure channel after a successful AUTH1; both counters start at 1 (section 8.3.1.13, p. 58). */
struct SecureChannel {
	Reader::SessionKeys mKeys{};
	uint32_t mReaderCounter{ 1 };
	/* AUTH1's response consumed device_counter 1. */
	uint32_t mDeviceCounter{ 2 };
	uint16_t mSignalingBitmap{ 0 };
};

/* SELECT, AUTH0, and AUTH1 in one NFC activation (section 8.2, p. 53); the field stays on. */
SecureChannel Authenticate(uint8_t groupFill)
{
	AliroUd::Nfc::PostFieldOn();
	SettleWorker();

	const auto select = Transceive(Reader::SelectCommand());
	zassert_equal(kSwSuccess, select.mStatusWord, "SELECT must succeed");
	Bytes proprietaryTlv{};
	zassert_true(Reader::ParseSelectResponse(select.mData, proprietaryTlv));

	Reader::Transaction transaction{};
	zassert_true(Reader::BeginTransaction(Filled16(groupFill), Filled16(0x5B), 0x01, proprietaryTlv, transaction));
	const auto auth0 = Transceive(Reader::Auth0Command(transaction));
	zassert_equal(kSwSuccess, auth0.mStatusWord, "AUTH0 must succeed");
	zassert_true(Reader::ParseAuth0Response(auth0.mData, transaction.mCredentialEphemeralPublicKey));

	Reader::Signature readerSignature{};
	zassert_true(Reader::Sign(HexArray<32>(Spec143::kReaderPrivateKey),
				  Reader::AuthenticationData(transaction, Reader::kReaderSignatureUsage), readerSignature));
	const auto auth1 = Transceive(Reader::Auth1Command(0x01, readerSignature, std::nullopt));
	zassert_equal(kSwSuccess, auth1.mStatusWord, "AUTH1 must succeed");

	SecureChannel channel{};
	zassert_true(Reader::DeriveSessionKeys(transaction, HexArray<65>(Spec143::kReaderPublicKey), channel.mKeys));
	Bytes payload{};
	zassert_true(Reader::DecryptDeviceResponse(channel.mKeys.mExpeditedSkDevice, 1, auth1.mData, payload));
	const auto bitmap = Reader::FindTlv(payload, 0x5E);
	zassert_true(bitmap.has_value() && bitmap->size() == 2, "AUTH1 payload must carry a 2-byte 0x5E");
	channel.mSignalingBitmap = static_cast<uint16_t>(((*bitmap)[0] << 8) | (*bitmap)[1]);
	return channel;
}

struct ExchangeResult {
	Reader::Response mResponse;
	/* Decrypted response payload; empty unless SW 9000 with data. */
	Bytes mPlaintext;
	Reader::ExchangeResponse mParsed;
};

/* Encrypts `plaintext`, sends it in an EXCHANGE command, and decrypts a 9000 response. */
ExchangeResult Exchange(SecureChannel &channel, const Bytes &plaintext, bool corruptTag = false)
{
	Bytes encrypted{};
	zassert_true(Reader::EncryptReaderCommand(channel.mKeys.mExpeditedSkReader, channel.mReaderCounter++,
						  plaintext, encrypted));
	if (corruptTag) {
		encrypted.back() ^= 0x01;
	}

	ExchangeResult result{};
	result.mResponse = Transceive(Reader::ExchangeCommand(encrypted));
	if (result.mResponse.mStatusWord == kSwSuccess && !result.mResponse.mData.empty()) {
		zassert_true(Reader::DecryptDeviceResponse(channel.mKeys.mExpeditedSkDevice, channel.mDeviceCounter++,
							   result.mResponse.mData, result.mPlaintext),
			     "EXCHANGE response must authenticate under ExpeditedSKDevice");
		zassert_true(Reader::ParseExchangeResponse(result.mPlaintext, result.mParsed),
			     "EXCHANGE response must be length-prefixed blocks ending in a status block");
	}
	return result;
}

ExchangeResult ExchangeMailbox(SecureChannel &channel, const Bytes &requests)
{
	return Exchange(channel, Reader::Exchange::MailboxCommands(requests));
}

Bytes Concat(std::initializer_list<Bytes> parts)
{
	Bytes out{};
	for (const auto &part : parts) {
		out.insert(out.end(), part.begin(), part.end());
	}
	return out;
}

void AssertExchangeSucceeded(const ExchangeResult &result, size_t expectedReadBlocks, const char *message)
{
	zassert_equal(kSwSuccess, result.mResponse.mStatusWord, "%s: SW must be 9000", message);
	zassert_equal(kExchangeSuccess, result.mParsed.mStatus, "%s: status must be 0x0002||0000", message);
	zassert_equal(expectedReadBlocks, result.mParsed.mReadData.size(), "%s: read data blocks", message);
}

/*
 * Section 8.3.3.5.5, p. 87: SW 9000 with exactly `0x0002 || B1 || B2`,
 * B1B2 != 0000, and no other data (B1/B2 are implementation-specific).
 */
void AssertExchangeErrorSequence(const ExchangeResult &result, const char *message)
{
	zassert_equal(kSwSuccess, result.mResponse.mStatusWord, "%s: SW must be 9000", message);
	zassert_equal(4u, result.mPlaintext.size(), "%s: payload must be exactly 0x0002||B1||B2", message);
	zassert_equal(0u, result.mParsed.mReadData.size(), "%s: no read data may be returned", message);
	zassert_not_equal(kExchangeSuccess, result.mParsed.mStatus, "%s: B1B2 must not be 0000", message);
}

/* Section 8.3.3.1, pp. 63-64: failure process — an error SW and an empty data field. */
void AssertFailureProcess(const Reader::Response &response, const char *message)
{
	zassert_not_equal(kSwSuccess, response.mStatusWord, "%s: SW must not be 9000", message);
	zassert_equal(0u, response.mData.size(), "%s: data field must be empty", message);
}

/* Section 10.2.2.1, p. 97, Test Plan Table 5-7, p. 48: CONTROL FLOW "failed, no information". */
void SendFailureControlFlow()
{
	const auto response = Transceive(Reader::ControlFlowCommand(0x00, 0x00));
	zassert_equal(kSwSuccess, response.mStatusWord, "CONTROL FLOW must return 9000");
	zassert_equal(0u, response.mData.size(), "CONTROL FLOW response data must be empty");
}

/* The stack ended the session itself: the transport was released and every mailbox snapshot closed. */
void AssertTransactionEnded(size_t expectedTerminations)
{
	SettleWorker();
	zassert_false(AliroUd::Nfc::IsSessionActive(), "The stack must have terminated the NFC session");
	zassert_equal(expectedTerminations, FakeNfc::GetTerminationCount(), "Unexpected transport terminations");
	zassert_equal(0u, AliroUd::Mailbox::Sessions::GetOpenSessionCount(), "No mailbox session may stay open");
}

void AssertSingleOutcome(const char *outcome)
{
	zassert_equal(1u, log_capture_count(outcome), "Expected exactly one %s notification", outcome);
	zassert_equal(1u, log_capture_total(), "Exactly one transaction result must be notified");
}

void EndTransaction()
{
	AliroUd::Nfc::PostFieldOff();
	SettleWorker();
}

void *SetupSuite(void)
{
	AliroUd::Nfc::StartWorker();
	SettleWorker();
	return nullptr;
}

void ResetBeforeEachTest(void *fixture)
{
	ARG_UNUSED(fixture);

	if (AliroUd::Nfc::IsSessionActive()) {
		EndTransaction();
	}

	FakeNfc::Reset();
	AliroUd::Credential::Test::ResetFakePersistence();
	AliroUd::Credential::Test::ResetFakeKeyBackend();
	AliroUd::PersistentKey::Test::ResetFakePersistentKeyPersistence();
	AliroUd::PersistentKey::Test::ResetFakePersistentKeyBackend();
	AliroUd::Mailbox::Test::ResetFakeMailboxPersistence();
	AliroUd::Authorization::GlobalWindow().Close();
	AliroUd::Authorization::Test::ResetFakeAuthorizationIndicator();

	/* Boot order from main.cpp. */
	zassert_equal(ALIRO_NO_ERROR, AliroUd::Credential::Store::Init());
	zassert_equal(ALIRO_NO_ERROR, AliroUd::PersistentKey::Store::Init());
	zassert_equal(ALIRO_NO_ERROR, AliroUd::Mailbox::Store::Init());

	log_capture_reset();
}

} // namespace

ZTEST_SUITE(aliro_ud_exchange_lifecycle, nullptr, SetupSuite, ResetBeforeEachTest, nullptr, nullptr);

/**
 * @brief Authorized reads return committed mailbox data, and a final Reader
 * success status (Table 8-18, p. 84; Test Plan Table 5-5, p. 47) ends the
 * transaction as `success` exactly once, with the transport and mailbox
 * released (P1-026, P1-029, P1-032, P1-033, P1-036, P1-037).
 */
ZTEST(aliro_ud_exchange_lifecycle, test_read_then_final_success_status_ends_transaction)
{
	const auto handle = ProvisionWithMailbox(0x31, {});
	const Bytes seed = SeedPattern();
	SeedMailbox(handle, seed);

	auto channel = Authenticate(0x31);
	zassert_equal(0x0038, channel.mSignalingBitmap, "Expected mailbox data (bit3) and read/write rights (bits 4/5)");

	const auto read = ExchangeMailbox(channel, Concat({ Reader::Exchange::AtomicSession(false),
							    Reader::Exchange::ReadRequest(0, 4),
							    Reader::Exchange::ReadRequest(10, 6) }));
	AssertExchangeSucceeded(read, 2, "Mailbox reads");
	AssertBytesEqual(Bytes(seed.begin(), seed.begin() + 4), read.mParsed.mReadData[0], "First read block");
	AssertBytesEqual(Bytes(seed.begin() + 10, seed.begin() + 16), read.mParsed.mReadData[1], "Second read block");
	zassert_equal(0u, log_capture_total(), "No outcome may be notified before the final Reader status");

	const auto status = Exchange(channel, Reader::Exchange::ReaderStatus(0x01, 0x00));
	AssertExchangeSucceeded(status, 0, "Final Reader success status");

	AssertTransactionEnded(1);
	AssertSingleOutcome("outcome=success");
	AssertBytesEqual(seed, CommittedMailbox(handle), "Reads must not change the mailbox");

	EndTransaction();
	zassert_equal(1u, log_capture_total(), "Field off after termination must not notify again");
}

/**
 * @brief Write and set requests in one command outside an atomic session
 * are committed together before the next command (section 8.3.3.5.4, p. 87;
 * Test Plan 7.28/7.29, pp. 78-79) (P1-033, P1-034).
 */
ZTEST(aliro_ud_exchange_lifecycle, test_write_and_set_commit_within_one_command)
{
	const auto handle = ProvisionWithMailbox(0x32, {});
	Bytes expected(kMailboxSize, 0x00);
	SeedMailbox(handle, expected);

	auto channel = Authenticate(0x32);

	const Bytes written{ 0xAA, 0xBB, 0xCC };
	const auto write = ExchangeMailbox(channel, Concat({ Reader::Exchange::AtomicSession(false),
							     Reader::Exchange::WriteRequest(2, written),
							     Reader::Exchange::SetRequest(8, 4, 0x5A) }));
	AssertExchangeSucceeded(write, 0, "Write and set");
	std::copy(written.begin(), written.end(), expected.begin() + 2);
	std::fill_n(expected.begin() + 8, 4, 0x5A);
	AssertBytesEqual(expected, CommittedMailbox(handle), "Write and set must be committed after their command");

	const auto read = ExchangeMailbox(
		channel, Concat({ Reader::Exchange::AtomicSession(false), Reader::Exchange::ReadRequest(0, 12) }));
	AssertExchangeSucceeded(read, 1, "Read back");
	AssertBytesEqual(Bytes(expected.begin(), expected.begin() + 12), read.mParsed.mReadData[0],
			 "Read back must return the committed data");

	AssertExchangeSucceeded(Exchange(channel, Reader::Exchange::ReaderStatus(0x01, 0x00)), 0, "Final status");
	AssertTransactionEnded(1);
	AssertSingleOutcome("outcome=success");
}

/**
 * @brief Writes and sets inside an atomic session leave the pre-session
 * data readable and persistent until the command that stops the session;
 * that command's reads still return non-updated data, and later reads
 * return the final data (Table 8-16, p. 82; Test Plan 7.28, p. 78) (P1-034).
 */
ZTEST(aliro_ud_exchange_lifecycle, test_atomic_session_commits_only_when_closed)
{
	const auto handle = ProvisionWithMailbox(0x33, {});
	const Bytes seed = SeedPattern();
	SeedMailbox(handle, seed);

	auto channel = Authenticate(0x33);

	AssertExchangeSucceeded(ExchangeMailbox(channel, Concat({ Reader::Exchange::AtomicSession(true),
								  Reader::Exchange::WriteRequest(0, { 0x11, 0x22 }) })),
				0, "Atomic write");
	AssertBytesEqual(seed, CommittedMailbox(handle), "An open atomic session must not commit");
	zassert_equal(1u, AliroUd::Mailbox::Sessions::GetOpenSessionCount(), "The atomic session holds one snapshot");

	const auto stagedRead = ExchangeMailbox(channel, Concat({ Reader::Exchange::AtomicSession(true),
								  Reader::Exchange::SetRequest(4, 2, 0x33),
								  Reader::Exchange::ReadRequest(0, 6) }));
	AssertExchangeSucceeded(stagedRead, 1, "Atomic set and read");
	AssertBytesEqual(Bytes(seed.begin(), seed.begin() + 6), stagedRead.mParsed.mReadData[0],
			 "Reads inside an atomic session must return non-updated data");
	AssertBytesEqual(seed, CommittedMailbox(handle), "An open atomic session must not commit");

	const auto closing = ExchangeMailbox(
		channel, Concat({ Reader::Exchange::AtomicSession(false), Reader::Exchange::ReadRequest(0, 6) }));
	AssertExchangeSucceeded(closing, 1, "Atomic session stop");
	AssertBytesEqual(Bytes(seed.begin(), seed.begin() + 6), closing.mParsed.mReadData[0],
			 "The closing command's reads must return non-updated data");

	Bytes expected = seed;
	expected[0] = 0x11;
	expected[1] = 0x22;
	expected[4] = 0x33;
	expected[5] = 0x33;
	AssertBytesEqual(expected, CommittedMailbox(handle), "Stopping the atomic session must commit every change");

	const auto after = ExchangeMailbox(
		channel, Concat({ Reader::Exchange::AtomicSession(false), Reader::Exchange::ReadRequest(0, 6) }));
	AssertExchangeSucceeded(after, 1, "Read after commit");
	AssertBytesEqual(Bytes(expected.begin(), expected.begin() + 6), after.mParsed.mReadData[0],
			 "Reads after the atomic session must return the final data");

	AssertExchangeSucceeded(Exchange(channel, Reader::Exchange::ReaderStatus(0x01, 0x00)), 0, "Final status");
	AssertTransactionEnded(1);
	AssertSingleOutcome("outcome=success");
}

/**
 * @brief An out-of-bounds request produces the in-channel error sequence,
 * nothing from that command is committed, and the Reader's CONTROL FLOW
 * ends the transaction as `failed` (section 8.3.3.5.5, p. 87; Test Plan
 * 7.33, p. 83) (P1-035, P1-036, P1-037).
 */
ZTEST(aliro_ud_exchange_lifecycle, test_out_of_bounds_request_fails_without_partial_commit)
{
	const auto handle = ProvisionWithMailbox(0x34, {});
	const Bytes seed = SeedPattern();
	SeedMailbox(handle, seed);

	auto channel = Authenticate(0x34);

	const auto result = ExchangeMailbox(channel, Concat({ Reader::Exchange::AtomicSession(false),
							      Reader::Exchange::WriteRequest(0, { 0xEE }),
							      Reader::Exchange::ReadRequest(kMailboxSize - 2, 4) }));
	AssertExchangeErrorSequence(result, "Out-of-bounds read");
	AssertBytesEqual(seed, CommittedMailbox(handle), "A failed command must not commit its write");
	zassert_equal(0u, log_capture_total(), "The outcome is concluded by CONTROL FLOW");

	SendFailureControlFlow();
	AssertTransactionEnded(1);
	AssertSingleOutcome("outcome=failed");
	AssertBytesEqual(seed, CommittedMailbox(handle), "The mailbox must be unchanged");
}

/**
 * @brief A write without the Reader's write right produces the error
 * sequence, after which the session keys are gone and a further EXCHANGE
 * is rejected (section 8.3.3.5.5, pp. 87-88) (P1-033, P1-035, P1-038).
 */
ZTEST(aliro_ud_exchange_lifecycle, test_unauthorized_write_fails_and_destroys_session_keys)
{
	const auto handle = ProvisionWithMailbox(0x35, kReadOnly);
	const Bytes seed = SeedPattern();
	SeedMailbox(handle, seed);

	auto channel = Authenticate(0x35);
	zassert_equal(0x0018, channel.mSignalingBitmap, "Expected mailbox data (bit3) and the read right only (bit4)");

	const auto write = ExchangeMailbox(
		channel, Concat({ Reader::Exchange::AtomicSession(false), Reader::Exchange::WriteRequest(0, { 0x01 }) }));
	AssertExchangeErrorSequence(write, "Unauthorized write");
	AssertBytesEqual(seed, CommittedMailbox(handle), "An unauthorized write must not change the mailbox");

	const auto afterError = ExchangeMailbox(
		channel, Concat({ Reader::Exchange::AtomicSession(false), Reader::Exchange::ReadRequest(0, 4) }));
	AssertFailureProcess(afterError.mResponse, "EXCHANGE after the error sequence");

	SendFailureControlFlow();
	AssertTransactionEnded(1);
	AssertSingleOutcome("outcome=failed");
}

/**
 * @brief A read without the Reader's read right produces the error
 * sequence (section 8.3.1.15, p. 60; section 8.3.3.5.5, p. 87) (P1-033,
 * P1-035).
 */
ZTEST(aliro_ud_exchange_lifecycle, test_unauthorized_read_fails)
{
	const auto handle = ProvisionWithMailbox(0x36, kWriteOnly);
	SeedMailbox(handle, SeedPattern());

	auto channel = Authenticate(0x36);

	const auto read = ExchangeMailbox(
		channel, Concat({ Reader::Exchange::AtomicSession(false), Reader::Exchange::ReadRequest(0, 4) }));
	AssertExchangeErrorSequence(read, "Unauthorized read");

	SendFailureControlFlow();
	AssertTransactionEnded(1);
	AssertSingleOutcome("outcome=failed");
}

/**
 * @brief A request tag with the wrong length produces the error sequence
 * (Test Plan 7.34, p. 84) (P1-035).
 */
ZTEST(aliro_ud_exchange_lifecycle, test_wrong_length_request_fails)
{
	const auto handle = ProvisionWithMailbox(0x37, {});
	const Bytes seed = SeedPattern();
	SeedMailbox(handle, seed);

	auto channel = Authenticate(0x37);

	/* A 0x87 read request carries offset(2) || length(2); this one is 3 bytes. */
	const Bytes shortRead{ 0x87, 0x03, 0x00, 0x00, 0x04 };
	const auto result = ExchangeMailbox(
		channel, Concat({ Reader::Exchange::AtomicSession(false), Reader::Exchange::WriteRequest(0, { 0xEE }),
				  shortRead }));
	AssertExchangeErrorSequence(result, "Wrong-length read request");
	AssertBytesEqual(seed, CommittedMailbox(handle), "A malformed command must not commit its write");

	SendFailureControlFlow();
	AssertTransactionEnded(1);
	AssertSingleOutcome("outcome=failed");
}

/**
 * @brief An EXCHANGE command whose authentication tag does not verify runs
 * the failure process and changes nothing (section 8.3.3.5.4, p. 87;
 * section 8.3.3.1, pp. 63-64) (P1-036, P1-038).
 */
ZTEST(aliro_ud_exchange_lifecycle, test_unauthenticated_exchange_runs_failure_process)
{
	const auto handle = ProvisionWithMailbox(0x38, {});
	const Bytes seed = SeedPattern();
	SeedMailbox(handle, seed);

	auto channel = Authenticate(0x38);

	const auto result = Exchange(channel,
				     Reader::Exchange::MailboxCommands(Concat(
					     { Reader::Exchange::AtomicSession(false),
					       Reader::Exchange::WriteRequest(0, { 0xEE }) })),
				     true);
	AssertFailureProcess(result.mResponse, "Tampered EXCHANGE");
	AssertBytesEqual(seed, CommittedMailbox(handle), "A tampered command must not change the mailbox");

	SendFailureControlFlow();
	AssertTransactionEnded(1);
	AssertSingleOutcome("outcome=failed");
}

/**
 * @brief A final Reader failure status (Table 8-18, p. 84; Test Plan
 * Table 5-6, p. 47) is acknowledged with `0x0002||0000` and ends the
 * transaction as `failed` (P1-037).
 */
ZTEST(aliro_ud_exchange_lifecycle, test_final_reader_failure_status_ends_transaction)
{
	const auto handle = ProvisionWithMailbox(0x39, {});
	SeedMailbox(handle, SeedPattern());

	auto channel = Authenticate(0x39);

	const auto status = Exchange(channel, Reader::Exchange::ReaderStatus(0x00, 0x04));
	AssertExchangeSucceeded(status, 0, "Final Reader failure status");

	AssertTransactionEnded(1);
	AssertSingleOutcome("outcome=failed");
}

/**
 * @brief After a failed AUTH1, the Reader's CONTROL FLOW (Table 8-2,
 * p. 64) is acknowledged and ends the transaction as `failed` before the
 * field goes away (P1-037, P1-038).
 */
ZTEST(aliro_ud_exchange_lifecycle, test_control_flow_after_auth1_failure_ends_transaction)
{
	ProvisionWithMailbox(0x3A, {});

	AliroUd::Nfc::PostFieldOn();
	SettleWorker();
	const auto select = Transceive(Reader::SelectCommand());
	zassert_equal(kSwSuccess, select.mStatusWord);
	Bytes proprietaryTlv{};
	zassert_true(Reader::ParseSelectResponse(select.mData, proprietaryTlv));
	Reader::Transaction transaction{};
	zassert_true(Reader::BeginTransaction(Filled16(0x3A), Filled16(0x5B), 0x01, proprietaryTlv, transaction));
	const auto auth0 = Transceive(Reader::Auth0Command(transaction));
	zassert_equal(kSwSuccess, auth0.mStatusWord);
	zassert_true(Reader::ParseAuth0Response(auth0.mData, transaction.mCredentialEphemeralPublicKey));

	/* Signed over the wrong usage, so the Reader signature does not verify. */
	Reader::Signature readerSignature{};
	zassert_true(Reader::Sign(HexArray<32>(Spec143::kReaderPrivateKey),
				  Reader::AuthenticationData(transaction, Reader::kUserDeviceSignatureUsage), readerSignature));
	AssertFailureProcess(Transceive(Reader::Auth1Command(0x01, readerSignature, std::nullopt)), "AUTH1");

	SendFailureControlFlow();
	AssertTransactionEnded(1);
	AssertSingleOutcome("outcome=failed");

	EndTransaction();
	zassert_equal(1u, log_capture_total(), "Field off after termination must not notify again");
}

enum class RejectedCommand { Auth0WrongP1P2, Auth0UnsupportedVersion, Auth1OutOfSequence, Auth1WrongP1P2 };

/* Runs SELECT and then `rejected`, which the stack must refuse with the failure process. */
Reader::Response RunRejectedCommand(uint8_t groupFill, RejectedCommand rejected)
{
	AliroUd::Nfc::PostFieldOn();
	SettleWorker();
	const auto select = Transceive(Reader::SelectCommand());
	zassert_equal(kSwSuccess, select.mStatusWord);
	Bytes proprietaryTlv{};
	zassert_true(Reader::ParseSelectResponse(select.mData, proprietaryTlv));
	Reader::Transaction transaction{};
	zassert_true(Reader::BeginTransaction(Filled16(groupFill), Filled16(0x5B), 0x01, proprietaryTlv, transaction));

	Reader::Signature readerSignature{};
	if (rejected == RejectedCommand::Auth1OutOfSequence) {
		zassert_true(Reader::Sign(HexArray<32>(Spec143::kReaderPrivateKey),
					  Reader::AuthenticationData(transaction, Reader::kReaderSignatureUsage),
					  readerSignature));
		return Transceive(Reader::Auth1Command(0x01, readerSignature, std::nullopt));
	}

	if (rejected == RejectedCommand::Auth0UnsupportedVersion) {
		transaction.mProtocolVersion = { 0xFF, 0xFF };
	}
	Bytes auth0 = Reader::Auth0Command(transaction);
	if (rejected == RejectedCommand::Auth0WrongP1P2) {
		auth0[2] = 0x01;
		auth0[3] = 0x01;
	}
	const auto auth0Response = Transceive(auth0);
	if (rejected != RejectedCommand::Auth1WrongP1P2) {
		return auth0Response;
	}

	zassert_equal(kSwSuccess, auth0Response.mStatusWord, "AUTH0 must succeed");
	zassert_true(Reader::ParseAuth0Response(auth0Response.mData, transaction.mCredentialEphemeralPublicKey));
	zassert_true(Reader::Sign(HexArray<32>(Spec143::kReaderPrivateKey),
				  Reader::AuthenticationData(transaction, Reader::kReaderSignatureUsage), readerSignature));
	Bytes auth1 = Reader::Auth1Command(0x01, readerSignature, std::nullopt);
	auth1[2] = 0x01;
	auth1[3] = 0x01;
	return Transceive(auth1);
}

/**
 * @brief Wrong-P1/P2, unsupported-version, and out-of-sequence AUTH0/AUTH1
 * commands (section 8.3.3.2.6, p. 69; section 8.3.3.4.5, p. 77; Test Plan
 * 7.19 and 7.24, pp. 71 and 75) run the failure process, and the Reader's
 * CONTROL FLOW ends each transaction as `failed` with the transport
 * released (P1-038, P1-039).
 */
ZTEST(aliro_ud_exchange_lifecycle, test_rejected_auth_commands_end_transaction)
{
	ProvisionWithMailbox(0x3D, {});

	const std::array<RejectedCommand, 4> cases{ RejectedCommand::Auth0WrongP1P2,
						    RejectedCommand::Auth0UnsupportedVersion,
						    RejectedCommand::Auth1OutOfSequence, RejectedCommand::Auth1WrongP1P2 };
	for (size_t i = 0; i < cases.size(); ++i) {
		AssertFailureProcess(RunRejectedCommand(0x3D, cases[i]), "Rejected AUTH command");
		SendFailureControlFlow();
		AssertTransactionEnded(i + 1);
		zassert_equal(i + 1, log_capture_count("outcome=failed"), "Case %zu must be notified as failed", i);
		zassert_equal(i + 1, log_capture_total(), "Case %zu must be notified exactly once", i);
		EndTransaction();
	}
}

/**
 * @brief Field loss during an atomic session discards the staged changes,
 * closes the snapshot, and ends the transaction as `aborted`; the next
 * transaction reads the original data and can open a new atomic session
 * (P1-034, P1-037).
 */
ZTEST(aliro_ud_exchange_lifecycle, test_field_loss_during_atomic_session_discards_staged_changes)
{
	const auto handle = ProvisionWithMailbox(0x3B, {});
	const Bytes seed = SeedPattern();
	SeedMailbox(handle, seed);

	auto channel = Authenticate(0x3B);
	AssertExchangeSucceeded(ExchangeMailbox(channel, Concat({ Reader::Exchange::AtomicSession(true),
								  Reader::Exchange::WriteRequest(0, { 0xDE, 0xAD }) })),
				0, "Atomic write");
	zassert_equal(1u, AliroUd::Mailbox::Sessions::GetOpenSessionCount());

	EndTransaction();
	zassert_false(AliroUd::Nfc::IsSessionActive());
	zassert_equal(0u, AliroUd::Mailbox::Sessions::GetOpenSessionCount(), "Field loss must close the snapshot");
	AssertBytesEqual(seed, CommittedMailbox(handle), "Field loss must discard staged changes");
	AssertSingleOutcome("outcome=aborted");

	const size_t terminationsBeforeRecovery = FakeNfc::GetTerminationCount();
	auto recovery = Authenticate(0x3B);
	const auto reopened = ExchangeMailbox(recovery, Concat({ Reader::Exchange::AtomicSession(true),
								 Reader::Exchange::ReadRequest(0, 4) }));
	AssertExchangeSucceeded(reopened, 1, "Recovery atomic session");
	AssertBytesEqual(Bytes(seed.begin(), seed.begin() + 4), reopened.mParsed.mReadData[0],
			 "Recovery must read the original data");
	AssertExchangeSucceeded(Exchange(recovery, Reader::Exchange::ReaderStatus(0x01, 0x00)), 0, "Final status");

	AssertTransactionEnded(terminationsBeforeRecovery + 1);
	zassert_equal(1u, log_capture_count("outcome=success"), "The recovery transaction must succeed");
	zassert_equal(2u, log_capture_total(), "Each transaction must be notified exactly once");
}

/**
 * @brief When the session watchdog expires during an atomic session, the
 * stack terminates the session through the OS timer adapter, the staged
 * changes are discarded, and a new activation recovers (P1-034, P1-037).
 */
ZTEST(aliro_ud_exchange_lifecycle, test_session_timeout_during_atomic_session_recovers)
{
	const auto handle = ProvisionWithMailbox(0x3C, {});
	const Bytes seed = SeedPattern();
	SeedMailbox(handle, seed);

	auto channel = Authenticate(0x3C);
	AssertExchangeSucceeded(ExchangeMailbox(channel, Concat({ Reader::Exchange::AtomicSession(true),
								  Reader::Exchange::SetRequest(0, 8, 0x00) })),
				0, "Atomic set");

	k_msleep(2 * kSessionTimeoutMs);
	AssertTransactionEnded(1);
	AssertBytesEqual(seed, CommittedMailbox(handle), "A timed-out atomic session must discard staged changes");
	AssertSingleOutcome("outcome=aborted");

	EndTransaction();
	auto recovery = Authenticate(0x3C);
	const auto read = ExchangeMailbox(
		recovery, Concat({ Reader::Exchange::AtomicSession(true), Reader::Exchange::ReadRequest(0, 8) }));
	AssertExchangeSucceeded(read, 1, "Recovery read");
	AssertBytesEqual(Bytes(seed.begin(), seed.begin() + 8), read.mParsed.mReadData[0],
			 "Recovery must read the original data");
	AssertExchangeSucceeded(Exchange(recovery, Reader::Exchange::ReaderStatus(0x01, 0x00)), 0, "Final status");

	AssertTransactionEnded(2);
	zassert_equal(1u, log_capture_count("outcome=success"), "The recovery transaction must succeed");
	zassert_equal(2u, log_capture_total(), "Each transaction must be notified exactly once");
}
