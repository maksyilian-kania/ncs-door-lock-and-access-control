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
#include "storage/mailbox/mailbox_store.h"
#include "test_reader.h"

#include <aliro/user_device/interface.h>

#include <array>
#include <cstring>
#include <optional>
#include <string>

/*
 * Certificate and no-certificate Expedited Standard transactions (Aliro 1.0
 * Specification and Test Plan, 26-42802-001, sections 8.2 and 8.3.3,
 * pp. 53-79) driven by the test Reader (common/test_reader.h) through the
 * real worker thread and the real Aliro::UserDeviceStack. The assertions
 * target the application-owned boundaries the stack reaches: transport
 * delivery, credential/trust lookup, authorization, credential signing, the
 * generic crypto adapter, document/mailbox/timestamp metadata, and
 * transaction notification. Protocol behavior itself is the stack's.
 */

using namespace AliroUdTest;
using AliroUdTest::Reader::Bytes;

namespace {

constexpr uint16_t kSwSuccess{ 0x9000 };
constexpr uint16_t kSwSecurityConditionNotSatisfied{ 0x6982 };

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
constexpr char kCredentialPublicKey[] = "0488f6f8f2f1e35a58879e72d9ea81957e8964c3d3c566eb9d41c83d0d8c63a23075dbdcf67d15bda429db38706"
					"a2f15ba90a2ac3c6a00973d21ed758c1471a748";
constexpr char kMockCredentialPrivateKey[] = "59f19aea46cba9ce209804b1c907d9f0dae520ce30152ac00a8307cc1dbe1199";
constexpr char kMockCredentialPublicKey[] = "04015731ebbb92ed25321ec99f55c4c00a85007eb0f8032a6e9a4163e2b542d326e244c8d7a42c6302bacfaaf"
					    "e3cc4daf3ff4d96880761c5a714f1d14664e48aec";
constexpr char kSelectResponseData[] = "6f158409a000000909acce5501a508800200005c020100";
constexpr char kReaderEphemeralPrivateKey[] = "3c0f74114cd2a021e8066efbaa31dbb97ef0054272192606fd96633a04f66214";
constexpr char kReaderEphemeralPublicKey[] = "049696afe33de58b7d3253d1cba86d14147c16d455e8a27373b38d454af21b70e75e13ebc6d55743ba6a6ffc4"
					     "ed37a55515a9346fdae311f60be30421fa6dc61c5";
constexpr char kTransactionIdentifier[] = "4165a83667ad0af5ab115247424822e0";
constexpr char kReaderIdentifier[] = "00112233445566778899aabbccddeeffffeeddccbbaa99887766554433221100";
constexpr char kAuth0ResponseData[] = "8641045d75ab60136a2c54ff27b799ee157f3f3329435c0df608de904c920ac29f72bd4274c2edc810a93e240"
				      "bf5d6394a92c9766b690b2bf5128ae70d6e29257ea786";
constexpr char kReaderAuthenticationData[] =
	"4d2000112233445566778899aabbccddeeffffeeddccbbaa9988776655443322110086205d75ab60136a2c54ff27b799ee157f3f33"
	"29435c0df608de904c920ac29f72bd87209696afe33de58b7d3253d1cba86d14147c16d455e8a27373b38d454af21b70e74c104165"
	"a83667ad0af5ab115247424822e09304415d9569";
constexpr char kReaderSignature[] = "501952e25339019804a7c3a7e4a1f6d993aec8baba7db6c8c20ac450428c2ff390c2188854ef7964927f8804"
				    "0dddf895ef57cce72379ad9688f36c5c7de3c294";
constexpr char kKdh[] = "cd227f01f917ad1dd5252db51c5ad3da1c3028be750a0f4e69c6a5624fca271c";
constexpr char kExpeditedSkReader[] = "f06ab1499102ca96f75cfa6d2e42c7920382d05a22e959325a91eb3aa4d71ce8";
constexpr char kExpeditedSkDevice[] = "de82f4f94575da8369febd52dea94ec3dadad6d4406a9efe76098d6a22a8fd5d";
constexpr char kAuth1ResponseData[] =
	"caae4715cb099959b6354df09a754bdeb31689e27be440d0c2cfe8d4e5b5d99ba367801c0f4f46485a160840f4e51b42d5b5e42015"
	"7d64188af6d89921ce5fa482f7e51725ba7568e5976cf6e9443fa57b32fd76a6a1b1b3190bd2aa0ee946f48c65dc8f3dc24c652fb9"
	"cab1381a68f0737a77c5e2b1cfbd9884041049d3e37b7126a2d74d7af03a322fbac65d627ef576a8d83e1a887b5be7";
constexpr char kAuth1ResponsePayload[] =
	"5a410488f6f8f2f1e35a58879e72d9ea81957e8964c3d3c566eb9d41c83d0d8c63a23075dbdcf67d15bda429db38706a2f15ba90a2"
	"ac3c6a00973d21ed758c1471a7489e402f57a5cb8a88c5a300fadb858d17298ed6f9dc01f9abc65e4b4089439868b8d24e93f1e54c"
	"a1df0703a76974a847ebafb42a7e90dccc3aaed788251d155a63e05e02003f";
} // namespace Spec143

/*
 * Aliro 1.0 Specification and Test Plan, 26-42802-001, section 14.1
 * "Reader certificate compression examples", "Demo 1", p. 169: the same
 * profile0000 vector crypto/src/test_certificate.cpp validates directly.
 */
namespace Demo1 {
constexpr char kReaderPrivateKey[] = "1a39e361b0db1915c2510bd92f3dbeb319ed68b16a0294347629d2e4becdb599";
constexpr char kIssuerPublicKey[] = "04793e3a8f20428d54e7318046d75d05a8737eb6e074e5146a207bff62dae90e24039f372814a312c3cb82a5a9"
				    "7bb5bfa9e623a3cc886b09dc13d53ef0da7de7bd";
constexpr char kCompressedCertificate[] =
	"308195040200003081"
	"8e854200048422"
	"42f6182ba1c1138d32b77fb9f7f37b70034b9f04443a5bea3c188beadb36490a7e95f91a4c162acfc3401c3a4f4e5a59251d45243a"
	"c8544a665cb951422f8648003045022100"
	"8720a2f08626d56b7814b7e5bbe04381e1834cf9a2a5d4c85c76783607a22cc60220236a4b757cd497c8570e84fa3221be99f6c78cc7"
	"cbc71d7328aa99be03f1eccf";
} // namespace Demo1

/* Dispatch settle time; the worker drains one event per wake. */
void SettleWorker()
{
	k_msleep(50);
}

Reader::PublicKey PublicKeyOf(const char *hex)
{
	return HexArray<65>(hex);
}

Reader::PrivateKey PrivateKeyOf(const char *hex)
{
	return HexArray<32>(hex);
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

struct CredentialSpec {
	Reader::PrivateKey mKey{};
	::Aliro::UserDevice::AuthenticationPolicy mPolicy{ ::Aliro::UserDevice::AuthenticationPolicy::UserDeviceSetting };
	std::array<uint8_t, 16> mReaderGroupIdentifier{};
	AliroUd::Credential::TrustType mTrustType{ AliroUd::Credential::TrustType::Direct };
	Reader::PublicKey mTrustKey{};
};

AliroUd::Credential::Provisioning::Payload PayloadFor(const CredentialSpec &spec)
{
	AliroUd::Credential::Provisioning::Payload payload{};
	payload.mHasNewKeyInput = true;
	payload.mNewKeyScalar = spec.mKey;
	payload.mPolicySet = true;
	payload.mPolicy = spec.mPolicy;
	payload.mBindingCount = 1;
	std::copy(spec.mReaderGroupIdentifier.begin(), spec.mReaderGroupIdentifier.end(),
		  payload.mBindings[0].mReaderGroupIdentifier.begin());
	payload.mBindings[0].mTrustType = spec.mTrustType;
	std::copy(spec.mTrustKey.begin(), spec.mTrustKey.end(), payload.mBindings[0].mKey.begin());
	return payload;
}

::Aliro::UserDevice::CredentialHandle Provision(const AliroUd::Credential::Provisioning::Payload &payload)
{
	::Aliro::UserDevice::CredentialHandle handle{ ::Aliro::UserDevice::kInvalidCredentialHandle };
	zassert_equal(ALIRO_NO_ERROR, AliroUd::Credential::Store::Create(payload, handle),
		      "Provisioning the credential must succeed");
	return handle;
}

::Aliro::UserDevice::CredentialHandle Provision(const CredentialSpec &spec)
{
	return Provision(PayloadFor(spec));
}

/* The §14.3 Access Credential bound, without a certificate, to the §14.3 Reader key. */
CredentialSpec NoCertCredential(uint8_t groupFill)
{
	CredentialSpec spec{};
	spec.mKey = PrivateKeyOf(Spec143::kCredentialPrivateKey);
	spec.mReaderGroupIdentifier = Filled16(groupFill);
	spec.mTrustType = AliroUd::Credential::TrustType::Direct;
	spec.mTrustKey = PublicKeyOf(Spec143::kReaderPublicKey);
	return spec;
}

/* The §14.3 Access Credential bound to the Demo 1 Reader System Issuer CA. */
CredentialSpec IssuerCredential(uint8_t groupFill)
{
	CredentialSpec spec{};
	spec.mKey = PrivateKeyOf(Spec143::kCredentialPrivateKey);
	spec.mReaderGroupIdentifier = Filled16(groupFill);
	spec.mTrustType = AliroUd::Credential::TrustType::IssuerCa;
	spec.mTrustKey = PublicKeyOf(Demo1::kIssuerPublicKey);
	return spec;
}

enum class CertificateMode { None, LoadCert, InAuth1 };

struct FlowOptions {
	std::array<uint8_t, 16> mReaderGroupIdentifier{};
	std::array<uint8_t, 16> mReaderGroupSubIdentifier{ Filled16(0x5B) };
	uint8_t mAuthenticationPolicy{ 0x01 };
	uint8_t mAuth1CommandParameters{ 0x01 };
	CertificateMode mCertificateMode{ CertificateMode::None };
	Bytes mCertificate;
	Reader::PrivateKey mReaderSigningKey{};
	/* The key the Reader expects to be bound to the reader_group_identifier (section 6.2, p. 28). */
	Reader::PublicKey mReaderGroupIdentifierKey{};
};

FlowOptions NoCertFlow(uint8_t groupFill)
{
	FlowOptions options{};
	options.mReaderGroupIdentifier = Filled16(groupFill);
	options.mReaderSigningKey = PrivateKeyOf(Spec143::kReaderPrivateKey);
	options.mReaderGroupIdentifierKey = PublicKeyOf(Spec143::kReaderPublicKey);
	return options;
}

FlowOptions CertFlow(uint8_t groupFill, CertificateMode mode)
{
	FlowOptions options{};
	options.mReaderGroupIdentifier = Filled16(groupFill);
	options.mCertificateMode = mode;
	options.mCertificate = Hex(Demo1::kCompressedCertificate);
	options.mReaderSigningKey = PrivateKeyOf(Demo1::kReaderPrivateKey);
	options.mReaderGroupIdentifierKey = PublicKeyOf(Demo1::kIssuerPublicKey);
	return options;
}

struct FlowResult {
	Reader::Transaction mTransaction;
	Reader::Response mSelect;
	Reader::Response mAuth0;
	std::optional<Reader::Response> mLoadCert;
	Reader::Response mAuth1;
	/* AUTH1 response payload, decrypted with the Reader-derived ExpeditedSKDevice; empty unless AUTH1 succeeded. */
	Bytes mAuth1Payload;
};

/*
 * SELECT, AUTH0, optional LOAD CERT, and AUTH1 in one NFC activation (section
 * 8.2, p. 53). The field stays on; callers end the transaction with
 * EndTransaction().
 */
FlowResult RunStandardFlow(const FlowOptions &options)
{
	FlowResult result{};

	AliroUd::Nfc::PostFieldOn();
	SettleWorker();

	result.mSelect = Transceive(Reader::SelectCommand());
	zassert_equal(kSwSuccess, result.mSelect.mStatusWord, "SELECT must succeed");
	Bytes proprietaryTlv{};
	zassert_true(Reader::ParseSelectResponse(result.mSelect.mData, proprietaryTlv),
		     "SELECT FCI must carry the 0xA5 proprietary information TLV");

	auto &transaction = result.mTransaction;
	zassert_true(Reader::BeginTransaction(options.mReaderGroupIdentifier, options.mReaderGroupSubIdentifier,
					      options.mAuthenticationPolicy, proprietaryTlv, transaction));

	result.mAuth0 = Transceive(Reader::Auth0Command(transaction));
	zassert_equal(kSwSuccess, result.mAuth0.mStatusWord, "AUTH0 must succeed");
	zassert_true(Reader::ParseAuth0Response(result.mAuth0.mData, transaction.mCredentialEphemeralPublicKey),
		     "AUTH0 response must be exactly one 0x86 credential_ePubK TLV");

	if (options.mCertificateMode == CertificateMode::LoadCert) {
		result.mLoadCert = Transceive(Reader::LoadCertCommand(options.mCertificate));
	}

	Reader::Signature readerSignature{};
	zassert_true(Reader::Sign(options.mReaderSigningKey,
				  Reader::AuthenticationData(transaction, Reader::kReaderSignatureUsage), readerSignature));
	const auto certificate = options.mCertificateMode == CertificateMode::InAuth1 ?
					 std::optional<Bytes>{ options.mCertificate } :
					 std::nullopt;
	result.mAuth1 = Transceive(Reader::Auth1Command(options.mAuth1CommandParameters, readerSignature, certificate));

	if (result.mAuth1.mStatusWord == kSwSuccess) {
		Reader::SessionKeys keys{};
		zassert_true(Reader::DeriveSessionKeys(transaction, options.mReaderGroupIdentifierKey, keys));
		zassert_true(Reader::DecryptDeviceResponse(keys.mExpeditedSkDevice, 1, result.mAuth1.mData,
							   result.mAuth1Payload),
			     "AUTH1 response must authenticate under the Reader-derived ExpeditedSKDevice");
	}

	return result;
}

void EndTransaction()
{
	AliroUd::Nfc::PostFieldOff();
	SettleWorker();
}

void AssertAuth1Rejected(const FlowResult &result, const char *message)
{
	zassert_equal(kSwSecurityConditionNotSatisfied, result.mAuth1.mStatusWord, "%s", message);
	zassert_equal(0u, result.mAuth1.mData.size(), "A failed AUTH1 must return an empty data field");
}

/* Verifies the AUTH1 User Device signature (Table 8-13, p. 77) against `credentialPublicKey`. */
void AssertDeviceSignatureValid(const FlowResult &result, const Reader::PublicKey &credentialPublicKey)
{
	const auto signatureTlv = Reader::FindTlv(result.mAuth1Payload, 0x9E);
	zassert_true(signatureTlv.has_value() && signatureTlv->size() == 64, "AUTH1 payload must carry 0x9E");
	Reader::Signature signature{};
	std::copy(signatureTlv->begin(), signatureTlv->end(), signature.begin());
	zassert_true(Reader::Verify(credentialPublicKey,
				    Reader::AuthenticationData(result.mTransaction, Reader::kUserDeviceSignatureUsage),
				    signature),
		     "User Device signature must verify with the selected Access Credential public key");
}

void AssertCredentialPublicKey(const FlowResult &result, const Reader::PublicKey &expected)
{
	const auto key = Reader::FindTlv(result.mAuth1Payload, 0x5A);
	zassert_true(key.has_value(), "AUTH1 payload must carry 0x5A when bit0 of command_parameters is set");
	zassert_equal(expected.size(), key->size());
	zassert_mem_equal(expected.data(), key->data(), expected.size());
	zassert_false(Reader::FindTlv(result.mAuth1Payload, 0x4E).has_value(), "key_slot must be absent");
}

uint16_t SignalingBitmap(const FlowResult &result)
{
	const auto bitmap = Reader::FindTlv(result.mAuth1Payload, 0x5E);
	zassert_true(bitmap.has_value() && bitmap->size() == 2, "AUTH1 payload must carry a 2-byte 0x5E");
	return static_cast<uint16_t>(((*bitmap)[0] << 8) | (*bitmap)[1]);
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

ZTEST_SUITE(aliro_ud_standard_integration, nullptr, SetupSuite, ResetBeforeEachTest, nullptr, nullptr);

/**
 * @brief The test Reader reproduces the section 14.3 example transcript
 * (pp. 175-177): authentication data, Kdh, ExpeditedSKReader/Device, and the
 * AUTH1 response decryption and signatures. This validates the verification
 * instrument itself, independently of the stack and the application.
 */
ZTEST(aliro_ud_standard_integration, test_reader_matches_spec_example_transcript)
{
	Bytes proprietaryTlv{};
	zassert_true(Reader::ParseSelectResponse(Hex(Spec143::kSelectResponseData), proprietaryTlv));
	const Bytes expectedProprietaryTlv = Hex("a508800200005c020100");
	zassert_equal(expectedProprietaryTlv.size(), proprietaryTlv.size());
	zassert_mem_equal(expectedProprietaryTlv.data(), proprietaryTlv.data(), proprietaryTlv.size());

	Reader::Transaction transaction{};
	transaction.mReaderIdentifier = HexArray<32>(Spec143::kReaderIdentifier);
	transaction.mTransactionIdentifier = HexArray<16>(Spec143::kTransactionIdentifier);
	transaction.mCommandParameters = 0x00;
	transaction.mAuthenticationPolicy = 0x01;
	transaction.mReaderEphemeralPrivateKey = PrivateKeyOf(Spec143::kReaderEphemeralPrivateKey);
	transaction.mReaderEphemeralPublicKey = PublicKeyOf(Spec143::kReaderEphemeralPublicKey);
	transaction.mProprietaryInformationTlv = proprietaryTlv;
	zassert_true(Reader::ParseAuth0Response(Hex(Spec143::kAuth0ResponseData),
						transaction.mCredentialEphemeralPublicKey));

	const Bytes readerData = Reader::AuthenticationData(transaction, Reader::kReaderSignatureUsage);
	const Bytes expectedReaderData = Hex(Spec143::kReaderAuthenticationData);
	zassert_equal(expectedReaderData.size(), readerData.size());
	zassert_mem_equal(expectedReaderData.data(), readerData.data(), readerData.size());
	zassert_true(Reader::Verify(PublicKeyOf(Spec143::kReaderPublicKey), readerData,
				    HexArray<64>(Spec143::kReaderSignature)));

	Reader::SessionKeys keys{};
	zassert_true(Reader::DeriveSessionKeys(transaction, PublicKeyOf(Spec143::kReaderPublicKey), keys));
	const auto expectedKdh = HexArray<32>(Spec143::kKdh);
	const auto expectedSkReader = HexArray<32>(Spec143::kExpeditedSkReader);
	const auto expectedSkDevice = HexArray<32>(Spec143::kExpeditedSkDevice);
	zassert_mem_equal(expectedKdh.data(), keys.mKdh.data(), keys.mKdh.size());
	zassert_mem_equal(expectedSkReader.data(), keys.mExpeditedSkReader.data(), keys.mExpeditedSkReader.size());
	zassert_mem_equal(expectedSkDevice.data(), keys.mExpeditedSkDevice.data(), keys.mExpeditedSkDevice.size());

	Bytes payload{};
	zassert_true(Reader::DecryptDeviceResponse(keys.mExpeditedSkDevice, 1, Hex(Spec143::kAuth1ResponseData), payload));
	const Bytes expectedPayload = Hex(Spec143::kAuth1ResponsePayload);
	zassert_equal(expectedPayload.size(), payload.size());
	zassert_mem_equal(expectedPayload.data(), payload.data(), payload.size());

	FlowResult result{};
	result.mTransaction = transaction;
	result.mAuth1Payload = payload;
	AssertDeviceSignatureValid(result, PublicKeyOf(Spec143::kCredentialPublicKey));

	Reader::PublicKey derived{};
	zassert_true(Reader::PublicKeyFor(PrivateKeyOf(Demo1::kReaderPrivateKey), derived));
	const Bytes certificate = Hex(Demo1::kCompressedCertificate);
	zassert_equal(152u, certificate.size());
	/* Demo 1 subject public key, as embedded in the compressed certificate at offset 13. */
	zassert_mem_equal(derived.data(), certificate.data() + 13, derived.size());
}

/**
 * @brief ALIRO-UD-SYRS-P1-017/018/019/022/025/026/027/028/029/031: a
 * no-certificate Expedited Standard transaction authenticates with the
 * directly-bound Reader key and returns, under the derived secure channel,
 * the Access Credential public key, a verifiable User Device signature, and
 * a signaling_bitmap with no capabilities for a credential without mailbox
 * or documents. Field loss afterwards is notified as an aborted transaction.
 */
ZTEST(aliro_ud_standard_integration, test_no_cert_flow_returns_credential_public_key)
{
	Provision(NoCertCredential(0xA1));

	const auto result = RunStandardFlow(NoCertFlow(0xA1));

	zassert_equal(kSwSuccess, result.mAuth1.mStatusWord, "AUTH1 must succeed");
	const auto credentialPublicKey = PublicKeyOf(Spec143::kCredentialPublicKey);
	AssertCredentialPublicKey(result, credentialPublicKey);
	AssertDeviceSignatureValid(result, credentialPublicKey);
	zassert_equal(0x0000, SignalingBitmap(result), "No document or mailbox capability may be advertised");
	zassert_false(Reader::FindTlv(result.mAuth1Payload, 0x4B).has_value());
	zassert_false(Reader::FindTlv(result.mAuth1Payload, 0x91).has_value());
	zassert_false(Reader::FindTlv(result.mAuth1Payload, 0x92).has_value());
	/* 0x5A (2 + 65) || 0x9E (2 + 64) || 0x5E (2 + 2): nothing else. */
	zassert_equal(137u, result.mAuth1Payload.size());
	zassert_equal(0u, AliroUd::Authorization::Test::GetSetActiveCallCount());

	EndTransaction();
	zassert_equal(1u, log_capture_count("outcome=aborted"), "Field loss must be notified as aborted");
	zassert_equal(1u, log_capture_total(), "Exactly one transaction result must be notified");
}

/**
 * @brief ALIRO-UD-SYRS-P1-028: with command_parameters bit0 clear, AUTH1
 * returns key_slot, the first 8 bytes of SHA-1 over the Access Credential
 * public key (section 8.3.3.4.2, p. 76), instead of the public key.
 */
ZTEST(aliro_ud_standard_integration, test_no_cert_flow_returns_key_slot)
{
	Provision(NoCertCredential(0xA2));

	auto options = NoCertFlow(0xA2);
	options.mAuth1CommandParameters = 0x00;
	const auto result = RunStandardFlow(options);

	zassert_equal(kSwSuccess, result.mAuth1.mStatusWord, "AUTH1 must succeed");
	const auto credentialPublicKey = PublicKeyOf(Spec143::kCredentialPublicKey);
	std::array<uint8_t, 8> expectedKeySlot{};
	zassert_true(Reader::KeySlot(credentialPublicKey, expectedKeySlot));
	const auto keySlot = Reader::FindTlv(result.mAuth1Payload, 0x4E);
	zassert_true(keySlot.has_value() && keySlot->size() == expectedKeySlot.size());
	zassert_mem_equal(expectedKeySlot.data(), keySlot->data(), expectedKeySlot.size());
	zassert_false(Reader::FindTlv(result.mAuth1Payload, 0x5A).has_value(), "Public key must be absent");
	AssertDeviceSignatureValid(result, credentialPublicKey);

	EndTransaction();
}

/**
 * @brief ALIRO-UD-SYRS-P1-018: every AUTH0 response carries a fresh
 * credential_ePubK (section 8.3.3.2.6, p. 69), generated through the
 * application's Crypto::GenerateEphemeralKeyPair() adapter.
 */
ZTEST(aliro_ud_standard_integration, test_auth0_ephemeral_key_is_fresh_per_transaction)
{
	Provision(NoCertCredential(0xA3));

	const auto first = RunStandardFlow(NoCertFlow(0xA3));
	EndTransaction();
	const auto second = RunStandardFlow(NoCertFlow(0xA3));
	EndTransaction();

	zassert_equal(kSwSuccess, first.mAuth1.mStatusWord);
	zassert_equal(kSwSuccess, second.mAuth1.mStatusWord);
	zassert_true(std::memcmp(first.mTransaction.mCredentialEphemeralPublicKey.data(),
				 second.mTransaction.mCredentialEphemeralPublicKey.data(),
				 first.mTransaction.mCredentialEphemeralPublicKey.size()) != 0,
		     "credential_ePubK must differ between transactions");
}

/**
 * @brief ALIRO-UD-SYRS-P1-019/022: the Access Credential and its Reader trust
 * are selected by the AUTH0 reader_group_identifier. A Reader key bound to a
 * different group does not authenticate this group.
 */
ZTEST(aliro_ud_standard_integration, test_credential_and_trust_follow_reader_group_identifier)
{
	const auto otherReaderKey = PrivateKeyOf(Demo1::kReaderPrivateKey);
	Reader::PublicKey otherReaderPublicKey{};
	zassert_true(Reader::PublicKeyFor(otherReaderKey, otherReaderPublicKey));

	Provision(NoCertCredential(0xB1));
	CredentialSpec second{};
	second.mKey = PrivateKeyOf(Spec143::kMockCredentialPrivateKey);
	second.mReaderGroupIdentifier = Filled16(0xB2);
	second.mTrustType = AliroUd::Credential::TrustType::Direct;
	second.mTrustKey = otherReaderPublicKey;
	Provision(second);

	auto options = NoCertFlow(0xB2);
	options.mReaderSigningKey = otherReaderKey;
	options.mReaderGroupIdentifierKey = otherReaderPublicKey;
	const auto selected = RunStandardFlow(options);
	EndTransaction();

	zassert_equal(kSwSuccess, selected.mAuth1.mStatusWord, "AUTH1 for the second group must succeed");
	const auto secondPublicKey = PublicKeyOf(Spec143::kMockCredentialPublicKey);
	AssertCredentialPublicKey(selected, secondPublicKey);
	AssertDeviceSignatureValid(selected, secondPublicKey);

	/* The §14.3 Reader key is bound only to group 0xB1. */
	const auto crossed = RunStandardFlow(NoCertFlow(0xB2));
	EndTransaction();
	AssertAuth1Rejected(crossed, "A Reader key bound to another group must not authenticate");
}

/**
 * @brief ALIRO-UD-SYRS-P1-022/031: a wrong Reader signature for a known
 * group and a correct-looking transaction for an unknown group are
 * externally indistinguishable (sections 8.3.3.2.6 and 8.3.3.4.6, pp. 69
 * and 78), and both are notified as failed transactions.
 */
ZTEST(aliro_ud_standard_integration, test_wrong_signature_is_indistinguishable_from_unknown_group)
{
	Provision(NoCertCredential(0xC1));

	auto wrongSigner = NoCertFlow(0xC1);
	wrongSigner.mReaderSigningKey = PrivateKeyOf(Demo1::kReaderPrivateKey);
	const auto wrongSignature = RunStandardFlow(wrongSigner);
	EndTransaction();

	const auto unknownGroup = RunStandardFlow(NoCertFlow(0xC2));
	EndTransaction();

	AssertAuth1Rejected(wrongSignature, "A wrong Reader signature must fail AUTH1");
	AssertAuth1Rejected(unknownGroup, "An unknown reader_group_identifier must fail AUTH1");
	zassert_equal(wrongSignature.mAuth0.mData.size(), unknownGroup.mAuth0.mData.size(),
		      "AUTH0 response shape must not depend on credential existence");
	zassert_equal(2u, log_capture_count("outcome=failed"), "Both failures must be notified as failed");
	zassert_equal(2u, log_capture_total());
}

/**
 * @brief ALIRO-UD-SYRS-P1-020: Reader policies 0x01 and 0x02 against a
 * credential policy of 0x01 proceed without an authorization window and
 * without an authentication-required indication.
 */
ZTEST(aliro_ud_standard_integration, test_user_device_setting_policies_proceed_without_window)
{
	Provision(NoCertCredential(0xD1));

	for (const uint8_t policy : { 0x01, 0x02 }) {
		auto options = NoCertFlow(0xD1);
		options.mAuthenticationPolicy = policy;
		const auto result = RunStandardFlow(options);
		EndTransaction();

		zassert_equal(kSwSuccess, result.mAuth1.mStatusWord, "Policy 0x%02x must proceed", policy);
		AssertDeviceSignatureValid(result, PublicKeyOf(Spec143::kCredentialPublicKey));
	}
	zassert_equal(0u, AliroUd::Authorization::Test::GetSetActiveCallCount());
}

/**
 * @brief ALIRO-UD-SYRS-P1-020/021: Reader policy 0x03 (section 8.3.1.14,
 * p. 60) continues only once the button authorization window is open.
 */
ZTEST(aliro_ud_standard_integration, test_force_user_authentication_requires_window)
{
	Provision(NoCertCredential(0xD2));

	auto options = NoCertFlow(0xD2);
	options.mAuthenticationPolicy = 0x03;
	const auto withoutWindow = RunStandardFlow(options);
	EndTransaction();
	AssertAuth1Rejected(withoutWindow, "Policy 0x03 without a window must not complete AUTH1");
	zassert_equal(1u, AliroUd::Authorization::Test::GetSetActiveCallCount(),
		      "Authentication-required must be indicated exactly once");

	AliroUd::Authorization::GlobalWindow().Open(k_uptime_get(), 30000);
	const auto withWindow = RunStandardFlow(options);
	EndTransaction();
	zassert_equal(kSwSuccess, withWindow.mAuth1.mStatusWord, "Policy 0x03 with a window must succeed");
	AssertDeviceSignatureValid(withWindow, PublicKeyOf(Spec143::kCredentialPublicKey));
	zassert_equal(1u, AliroUd::Authorization::Test::GetSetActiveCallCount());
}

/**
 * @brief ALIRO-UD-SYRS-P1-020/021: a credential provisioned with policy 0x03
 * requires the authorization window even when the Reader sends 0x01.
 */
ZTEST(aliro_ud_standard_integration, test_credential_force_policy_requires_window)
{
	auto spec = NoCertCredential(0xD3);
	spec.mPolicy = ::Aliro::UserDevice::AuthenticationPolicy::ForceUserAuthentication;
	Provision(spec);

	const auto withoutWindow = RunStandardFlow(NoCertFlow(0xD3));
	EndTransaction();
	AssertAuth1Rejected(withoutWindow, "Credential policy 0x03 without a window must not complete AUTH1");

	AliroUd::Authorization::GlobalWindow().Open(k_uptime_get(), 30000);
	const auto withWindow = RunStandardFlow(NoCertFlow(0xD3));
	EndTransaction();
	zassert_equal(kSwSuccess, withWindow.mAuth1.mStatusWord, "Credential policy 0x03 with a window must succeed");
}

/**
 * @brief ALIRO-UD-SYRS-P1-017/023/025/027: a reader certificate sent in
 * LOAD CERT is verified with the bound Reader System Issuer CA key, and its
 * subject key then authenticates the AUTH1 Reader signature. Session keys
 * use the issuer key as reader_group_identifier_key (section 6.2, p. 28).
 */
ZTEST(aliro_ud_standard_integration, test_certificate_in_load_cert_authenticates_reader)
{
	Provision(IssuerCredential(0xE1));

	const auto result = RunStandardFlow(CertFlow(0xE1, CertificateMode::LoadCert));
	EndTransaction();

	zassert_true(result.mLoadCert.has_value());
	zassert_equal(kSwSuccess, result.mLoadCert->mStatusWord, "LOAD CERT must succeed");
	zassert_equal(0u, result.mLoadCert->mData.size(), "LOAD CERT response data must be empty");
	zassert_equal(kSwSuccess, result.mAuth1.mStatusWord, "AUTH1 must succeed");
	const auto credentialPublicKey = PublicKeyOf(Spec143::kCredentialPublicKey);
	AssertCredentialPublicKey(result, credentialPublicKey);
	AssertDeviceSignatureValid(result, credentialPublicKey);
}

/** @brief ALIRO-UD-SYRS-P1-023: the same certificate carried in AUTH1 tag 0x90 (Table 8-10, p. 73). */
ZTEST(aliro_ud_standard_integration, test_certificate_in_auth1_authenticates_reader)
{
	Provision(IssuerCredential(0xE2));

	const auto result = RunStandardFlow(CertFlow(0xE2, CertificateMode::InAuth1));
	EndTransaction();

	zassert_equal(kSwSuccess, result.mAuth1.mStatusWord, "AUTH1 must succeed");
	const auto credentialPublicKey = PublicKeyOf(Spec143::kCredentialPublicKey);
	AssertCredentialPublicKey(result, credentialPublicKey);
	AssertDeviceSignatureValid(result, credentialPublicKey);
}

/**
 * @brief ALIRO-UD-SYRS-P1-024: an untrusted certificate fails at AUTH1 the
 * same way whether the group has a different issuer key or no issuer key
 * at all, and an invalidly encoded certificate is rejected by LOAD CERT.
 */
ZTEST(aliro_ud_standard_integration, test_untrusted_certificate_is_rejected_without_disclosing_trust)
{
	auto otherIssuer = IssuerCredential(0xE3);
	otherIssuer.mTrustKey = PublicKeyOf(Spec143::kReaderPublicKey);
	Provision(otherIssuer);
	Provision(NoCertCredential(0xE4));

	const auto wrongIssuer = RunStandardFlow(CertFlow(0xE3, CertificateMode::LoadCert));
	EndTransaction();
	const auto noIssuer = RunStandardFlow(CertFlow(0xE4, CertificateMode::LoadCert));
	EndTransaction();
	const auto wrongIssuerInAuth1 = RunStandardFlow(CertFlow(0xE3, CertificateMode::InAuth1));
	EndTransaction();

	zassert_equal(wrongIssuer.mLoadCert->mStatusWord, noIssuer.mLoadCert->mStatusWord,
		      "LOAD CERT must not disclose whether issuer trust exists");
	AssertAuth1Rejected(wrongIssuer, "A certificate from another issuer must fail AUTH1");
	AssertAuth1Rejected(noIssuer, "A certificate for a group without issuer trust must fail AUTH1");
	AssertAuth1Rejected(wrongIssuerInAuth1, "An AUTH1-carried untrusted certificate must fail AUTH1");

	Provision(IssuerCredential(0xE5));
	auto truncated = CertFlow(0xE5, CertificateMode::LoadCert);
	truncated.mCertificate.pop_back();
	const auto malformed = RunStandardFlow(truncated);
	EndTransaction();
	zassert_not_equal(kSwSuccess, malformed.mLoadCert->mStatusWord, "A malformed certificate must be rejected");
	zassert_equal(0u, malformed.mLoadCert->mData.size());
}

/**
 * @brief ALIRO-UD-SYRS-P1-029/030: AUTH1 advertises provisioned Access and
 * Revocation Documents and mailbox read/write rights in signaling_bitmap
 * (bits 0, 1, 4, 5; section 8.3.3.4.2, pp. 74-75) and returns the
 * provisioned credential_signed_timestamp and revocation_signed_timestamp.
 */
ZTEST(aliro_ud_standard_integration, test_auth1_reports_documents_mailbox_rights_and_timestamps)
{
	auto payload = PayloadFor(NoCertCredential(0xF1));
	payload.mMailbox.mConfigured = true;
	payload.mMailbox.mSizeBytes = 64;
	payload.mMailbox.mReadable = true;
	payload.mMailbox.mWritable = true;
	payload.mAccessDocument.mPresent = true;
	payload.mAccessDocument.mLength = 4;
	payload.mAccessDocument.mData[0] = 0xA1;
	payload.mRevocationDocument.mPresent = true;
	payload.mRevocationDocument.mLength = 4;
	payload.mRevocationDocument.mData[0] = 0xA1;
	constexpr char kCredentialTimestamp[] = "2026-09-25T12:00:00Z";
	constexpr char kRevocationTimestamp[] = "2026-09-24T08:30:00Z";
	payload.mHasCredentialSignedTimestamp = true;
	std::memcpy(payload.mCredentialSignedTimestamp.data(), kCredentialTimestamp,
		    payload.mCredentialSignedTimestamp.size());
	payload.mHasRevocationSignedTimestamp = true;
	std::memcpy(payload.mRevocationSignedTimestamp.data(), kRevocationTimestamp,
		    payload.mRevocationSignedTimestamp.size());
	Provision(payload);

	const auto result = RunStandardFlow(NoCertFlow(0xF1));
	EndTransaction();

	zassert_equal(kSwSuccess, result.mAuth1.mStatusWord, "AUTH1 must succeed");
	zassert_equal(0x0033, SignalingBitmap(result), "Expected documents (bits 0/1) and mailbox rights (bits 4/5)");
	const auto credentialTimestamp = Reader::FindTlv(result.mAuth1Payload, 0x91);
	const auto revocationTimestamp = Reader::FindTlv(result.mAuth1Payload, 0x92);
	zassert_true(credentialTimestamp.has_value() && credentialTimestamp->size() == 20);
	zassert_true(revocationTimestamp.has_value() && revocationTimestamp->size() == 20);
	zassert_mem_equal(kCredentialTimestamp, credentialTimestamp->data(), 20);
	zassert_mem_equal(kRevocationTimestamp, revocationTimestamp->data(), 20);
	zassert_false(Reader::FindTlv(result.mAuth1Payload, 0x4B).has_value(),
		      "No mailbox_data_subset is provisioned");
	AssertDeviceSignatureValid(result, PublicKeyOf(Spec143::kCredentialPublicKey));
}
