/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include <zephyr/kernel.h>
#include <zephyr/shell/shell.h>
#include <zephyr/shell/shell_dummy.h>
#include <zephyr/ztest.h>

#include "fake_credential_persistence.h"
#include "fake_key_backend.h"
#include "fake_mailbox_persistence.h"
#include "fake_persistent_key_backend.h"
#include "fake_persistent_key_persistence.h"
#include "platform/nfc/nfc_worker.h"
#include "storage/credential/credential_store.h"
#include "storage/credential/provisioning.h"
#include "storage/document/document_snapshots.h"
#include "storage/key/persistent_key_store.h"
#include "storage/mailbox/mailbox_store.h"

#include <aliro/user_device/user_device.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <string>

/*
 * Document provisioning through the development CLI, in front of the real
 * UserDeviceStack facade and the application's adapters and stores, with
 * the shared in-memory persistence fakes for fault injection. Provisioning
 * and lifecycle management are outside the Aliro specification (Aliro 1.0
 * Specification, section 6.2, page 28); an Access Credential includes its
 * optional mailbox and MAY be associated with an Access Document and a
 * Revocation Document (section 6.2, pages 28-29). Document bytes are opaque
 * CBOR here (section 7.2, page 32).
 */

using namespace Aliro;
using namespace Aliro::UserDevice;
using ::AliroUd::Credential::OptionalDocument;
using ::AliroUd::Credential::PersistedCredential;

namespace CredentialStore = ::AliroUd::Credential::Store;
namespace CredentialTest = ::AliroUd::Credential::Test;
namespace MailboxStore = ::AliroUd::Mailbox::Store;
namespace MailboxTest = ::AliroUd::Mailbox::Test;
namespace Provisioning = ::AliroUd::Credential::Provisioning;
namespace Snapshots = ::AliroUd::Document::Snapshots;

namespace {

constexpr size_t kMaxSize{ AliroUd::Credential::kDocumentMaxSizeBytes };
constexpr size_t kCredentials{ AliroUd::Credential::kMaxCredentials };
constexpr size_t kMailboxSize{ 8 };
constexpr size_t kReadChunk{ 128 };

/* Valid P-256 scalars (nonzero, below the curve order), differing in the last byte. */
constexpr const char *kKeyHex[]{
	"23231022a3662ceb6f2e6a4e998866ae88d6e9da1c72b050ae5c206a1da46712",
	"23231022a3662ceb6f2e6a4e998866ae88d6e9da1c72b050ae5c206a1da46713",
	"23231022a3662ceb6f2e6a4e998866ae88d6e9da1c72b050ae5c206a1da46714",
};
constexpr const char *kGroupHex{ "0102030405060708090a0b0c0d0e0f10" };
constexpr const char *kTrustKeyHex{ "04"
				    "1111111111111111111111111111111111111111111111111111111111111111"
				    "2222222222222222222222222222222222222222222222222222222222222222" };

const OptionalDocument kAbsent{};

/* One credential's committed state: record, mailbox configuration, and mailbox bytes. */
struct State {
	bool mExists{ false };
	PersistedCredential mRecord{};
	bool mMailboxInitialized{ false };
	std::array<uint8_t, kMailboxSize> mMailbox{};
};

std::array<State, 4> sBefore{};
State sActual{};
PersistedCredential sRecord{};

std::string Run(const std::string &command)
{
	const struct shell *sh = shell_backend_dummy_get_ptr();

	shell_backend_dummy_clear_output(sh);
	(void)shell_execute_cmd(sh, command.c_str());

	size_t size{ 0 };
	const char *buf = shell_backend_dummy_get_output(sh, &size);
	std::string output(buf, size);
	while (!output.empty() && std::isspace(static_cast<unsigned char>(output.back()))) {
		output.pop_back();
	}
	size_t start{ 0 };
	while (start < output.size() && std::isspace(static_cast<unsigned char>(output[start]))) {
		++start;
	}
	return output.substr(start);
}

void Expect(const std::string &command, const std::string &expected)
{
	const std::string output{ Run(command) };
	zassert_true(output == expected, "'%s' printed '%s', expected '%s'", command.c_str(), output.c_str(),
		     expected.c_str());
}

std::string ErrToken(const char *token, const char *command)
{
	return std::string("ERR ") + token + " command=" + command;
}

std::string ErrCode(AliroError error, const char *command)
{
	return "ERR " + std::to_string(error.ToInt()) + " command=" + command;
}

std::string Hex(const uint8_t *data, size_t length)
{
	static const char kDigits[]{ "0123456789abcdef" };
	std::string hex;
	for (size_t i = 0; i < length; ++i) {
		hex += kDigits[data[i] >> 4];
		hex += kDigits[data[i] & 0x0f];
	}
	return hex;
}

template <size_t N> void FromHex(const char *hex, std::array<uint8_t, N> &out)
{
	zassert_equal(2 * N, strlen(hex));
	for (size_t i = 0; i < N; ++i) {
		unsigned value{};
		zassert_equal(1, sscanf(hex + 2 * i, "%2x", &value));
		out[i] = static_cast<uint8_t>(value);
	}
}

OptionalDocument MakeDocument(size_t length, uint8_t seed)
{
	OptionalDocument document{};
	document.mPresent = true;
	document.mLength = static_cast<uint32_t>(length);
	for (size_t i = 0; i < length; ++i) {
		document.mData[i] = static_cast<uint8_t>(seed + 31 * i);
	}
	return document;
}

std::string SetDocument(const char *type, const OptionalDocument &document)
{
	return std::string("aliro-ud credential set-document ") + type + " " + std::to_string(document.mLength) + " " +
	       Hex(document.mData.data(), document.mLength);
}

bool SameDocument(const OptionalDocument &a, const OptionalDocument &b)
{
	return a.mPresent == b.mPresent && a.mLength == b.mLength && a.mData == b.mData;
}

std::string HandleText(CredentialHandle handle)
{
	return std::to_string(handle);
}

void BeginCreate(size_t key)
{
	Expect("aliro-ud credential begin-create", "OK");
	Expect(std::string("aliro-ud credential set-key ") + kKeyHex[key], "OK");
	Expect(std::string("aliro-ud credential set-binding 0 ") + kGroupHex + " direct " + kTrustKeyHex, "OK");
	Expect("aliro-ud credential set-mailbox 8 3", "OK");
}

CredentialHandle Commit()
{
	const std::string output{ Run("aliro-ud credential commit") };
	unsigned handle{ 0 };
	zassert_equal(1, sscanf(output.c_str(), "OK handle=%u", &handle), "commit failed: %s", output.c_str());
	return static_cast<CredentialHandle>(handle);
}

CredentialHandle Create(size_t key, const OptionalDocument &access, const OptionalDocument &revocation)
{
	BeginCreate(key);
	if (access.mPresent) {
		Expect(SetDocument("access", access), "OK");
	}
	if (revocation.mPresent) {
		Expect(SetDocument("revocation", revocation), "OK");
	}
	return Commit();
}

size_t CountCredentials()
{
	size_t count{ 0 };
	for (CredentialHandle handle = 1; handle <= kCredentials; ++handle) {
		CredentialMetadata metadata{};
		if (UserDeviceStack::Instance().GetCredentialMetadata(handle, metadata) == ALIRO_NO_ERROR) {
			++count;
		}
	}
	return count;
}

void ExpectCliBytes(CredentialHandle handle, const char *type, const OptionalDocument &expected)
{
	const std::string prefix{ "aliro-ud document read " + HandleText(handle) + " " + type + " " };

	if (!expected.mPresent) {
		Expect(prefix + "0 0", ErrToken("NOT_PRESENT", "document read"));
		return;
	}

	for (size_t offset = 0; offset < expected.mLength; offset += kReadChunk) {
		const size_t length = std::min<size_t>(kReadChunk, expected.mLength - offset);
		Expect(prefix + std::to_string(offset) + " " + std::to_string(length),
		       "OK data=" + Hex(expected.mData.data() + offset, length));
	}
	Expect(prefix + std::to_string(expected.mLength) + " 1", ErrToken("INVALID_ARGUMENT", "document read"));
}

/* The committed documents, byte for byte including unused storage, through the store and the CLI. */
void ExpectDocuments(CredentialHandle handle, const OptionalDocument &access, const OptionalDocument &revocation)
{
	zassert_equal(ALIRO_NO_ERROR, CredentialStore::GetFullRecord(handle, sRecord));
	zassert_true(SameDocument(access, sRecord.mAccessDocument), "committed Access Document differs");
	zassert_true(SameDocument(revocation, sRecord.mRevocationDocument), "committed Revocation Document differs");

	Expect("aliro-ud document inspect " + HandleText(handle),
	       "OK handle=" + HandleText(handle) + " access=" + std::to_string(access.mPresent ? 1 : 0) +
		       " access_length=" + std::to_string(access.mLength) +
		       " revocation=" + std::to_string(revocation.mPresent ? 1 : 0) +
		       " revocation_length=" + std::to_string(revocation.mLength));
	ExpectCliBytes(handle, "access", access);
	ExpectCliBytes(handle, "revocation", revocation);

	zassert_equal(0u, Snapshots::GetOpenSnapshotCount(), "inspection holds no snapshot");
}

void ExpectNothingStaged()
{
	Expect("aliro-ud credential set-document access 1 00", ErrToken("NO_TRANSACTION", "credential set-document"));
	Expect("aliro-ud credential commit", ErrToken("NO_TRANSACTION", "credential commit"));
	zassert_equal(0u, Snapshots::GetOpenSnapshotCount());
}

void FillMailbox(CredentialHandle handle, uint8_t seed)
{
	static std::array<uint8_t, AliroUd::Mailbox::kMaxSizeBytes> data{};
	static std::array<bool, AliroUd::Mailbox::kMaxSizeBytes> dirty{};
	data.fill(0);
	dirty.fill(false);
	for (size_t i = 0; i < kMailboxSize; ++i) {
		data[i] = static_cast<uint8_t>(seed + i);
		dirty[i] = true;
	}

	zassert_equal(ALIRO_NO_ERROR, MailboxStore::Initialize(handle));
	zassert_equal(ALIRO_NO_ERROR, MailboxStore::ApplyDirtyBytes(handle, data, dirty));
}

void Capture(CredentialHandle handle, State &out)
{
	out = State{};
	out.mExists = CredentialStore::GetFullRecord(handle, out.mRecord) == ALIRO_NO_ERROR;
	out.mMailboxInitialized = MailboxStore::IsInitialized(handle);
	if (out.mExists && out.mMailboxInitialized) {
		zassert_equal(ALIRO_NO_ERROR, MailboxStore::RawRead(handle, 0, out.mMailbox.data(), kMailboxSize));
	}
}

void ExpectState(CredentialHandle handle, const State &expected)
{
	Capture(handle, sActual);
	const auto &a = sActual.mRecord;
	const auto &e = expected.mRecord;

	zassert_equal(expected.mExists, sActual.mExists, "credential %u existence changed", handle);
	if (expected.mExists) {
		zassert_equal(e.mKeyId, a.mKeyId, "credential %u key changed", handle);
		zassert_true(e.mPublicKey == a.mPublicKey, "credential %u public key changed", handle);
		zassert_equal(e.mPolicy, a.mPolicy, "credential %u policy changed", handle);
		zassert_equal(e.mBindingCount, a.mBindingCount);
		zassert_true(e.mBindings[0].mReaderGroupIdentifier == a.mBindings[0].mReaderGroupIdentifier);
		zassert_true(e.mBindings[0].mKey == a.mBindings[0].mKey);
		zassert_equal(e.mMailbox.mConfigured, a.mMailbox.mConfigured);
		zassert_equal(e.mMailbox.mSizeBytes, a.mMailbox.mSizeBytes);
		zassert_true(SameDocument(e.mAccessDocument, a.mAccessDocument), "credential %u Access Document changed",
			     handle);
		zassert_true(SameDocument(e.mRevocationDocument, a.mRevocationDocument),
			     "credential %u Revocation Document changed", handle);
	}
	zassert_equal(expected.mMailboxInitialized, sActual.mMailboxInitialized, "credential %u mailbox state changed",
		      handle);
	zassert_true(expected.mMailbox == sActual.mMailbox, "credential %u mailbox bytes changed", handle);
}

void ExpectDeleted(CredentialHandle handle)
{
	zassert_equal(ALIRO_INVALID_ARGUMENT, CredentialStore::GetFullRecord(handle, sRecord));
	zassert_false(MailboxStore::IsInitialized(handle), "a deleted credential's mailbox is erased");
	Expect("aliro-ud mailbox inspect " + HandleText(handle), ErrCode(ALIRO_INVALID_ARGUMENT, "mailbox inspect"));
	Expect("aliro-ud document inspect " + HandleText(handle), ErrCode(ALIRO_INVALID_ARGUMENT, "document inspect"));
}

/* Boot order from main.cpp; the fakes keep what was persisted. */
void Reboot()
{
	zassert_equal(0u, Snapshots::GetOpenSnapshotCount());
	zassert_equal(ALIRO_NO_ERROR, CredentialStore::Init());
	zassert_equal(ALIRO_NO_ERROR, AliroUd::PersistentKey::Store::Init());
	zassert_equal(ALIRO_NO_ERROR, MailboxStore::Init());
}

void *SetupCli(void)
{
	const struct shell *sh = shell_backend_dummy_get_ptr();

	WAIT_FOR(shell_ready(sh), 20000, k_msleep(1));
	zassert_true(shell_ready(sh), "timed out waiting for dummy shell backend");

	AliroUd::Nfc::StartWorker();
	k_msleep(50);
	return nullptr;
}

/* Every case starts from empty storage with no open staging transaction. */
void ResetBeforeEachTest(void *fixture)
{
	ARG_UNUSED(fixture);

	(void)Run("aliro-ud credential abort");
	CredentialTest::ResetFakePersistence();
	CredentialTest::ResetFakeKeyBackend();
	AliroUd::PersistentKey::Test::ResetFakePersistentKeyPersistence();
	AliroUd::PersistentKey::Test::ResetFakePersistentKeyBackend();
	MailboxTest::ResetFakeMailboxPersistence();
	Reboot();
}

} // namespace

ZTEST_SUITE(aliro_ud_provisioning, nullptr, SetupCli, ResetBeforeEachTest, nullptr, nullptr);

/* Access-only, Revocation-only, both, and neither, through create and update transactions. */
ZTEST(aliro_ud_provisioning, test_documents_through_create_and_update)
{
	zassert_true(kCredentials >= 4, "one credential per document combination");

	static OptionalDocument accessOnly{};
	static OptionalDocument revocationOnly{};
	static OptionalDocument largest{};
	static OptionalDocument smallest{};
	static OptionalDocument replacement{};
	accessOnly = MakeDocument(16, 0x10);
	revocationOnly = MakeDocument(24, 0x40);
	largest = MakeDocument(kMaxSize, 0x11);
	smallest = MakeDocument(1, 0x41);
	replacement = MakeDocument(40, 0x70);

	const auto neither = Create(0, kAbsent, kAbsent);
	const auto access = Create(1, accessOnly, kAbsent);
	const auto revocation = Create(2, kAbsent, revocationOnly);
	const auto both = Create(0, largest, smallest);
	ExpectDocuments(neither, kAbsent, kAbsent);
	ExpectDocuments(access, accessOnly, kAbsent);
	ExpectDocuments(revocation, kAbsent, revocationOnly);
	ExpectDocuments(both, largest, smallest);

	/* An update without document setters keeps the committed documents. */
	Expect("aliro-ud credential begin-update " + HandleText(access), "OK");
	Expect("aliro-ud credential set-policy 2", "OK");
	Expect("aliro-ud credential commit", "OK handle=" + HandleText(access));
	ExpectDocuments(access, accessOnly, kAbsent);

	Expect("aliro-ud credential begin-update " + HandleText(neither), "OK");
	Expect(SetDocument("access", accessOnly), "OK");
	Expect(SetDocument("revocation", revocationOnly), "OK");
	Expect("aliro-ud credential commit", "OK handle=" + HandleText(neither));

	Expect("aliro-ud credential begin-update " + HandleText(access), "OK");
	Expect(SetDocument("revocation", smallest), "OK");
	Expect("aliro-ud credential commit", "OK handle=" + HandleText(access));

	Expect("aliro-ud credential begin-update " + HandleText(revocation), "OK");
	Expect("aliro-ud credential clear-document revocation", "OK");
	Expect(SetDocument("access", replacement), "OK");
	Expect("aliro-ud credential commit", "OK handle=" + HandleText(revocation));

	Expect("aliro-ud credential begin-update " + HandleText(both), "OK");
	Expect("aliro-ud credential clear-document access", "OK");
	Expect("aliro-ud credential clear-document revocation", "OK");
	Expect("aliro-ud credential commit", "OK handle=" + HandleText(both));

	for (int boot = 0; boot < 2; ++boot) {
		ExpectDocuments(neither, accessOnly, revocationOnly);
		ExpectDocuments(access, accessOnly, smallest);
		ExpectDocuments(revocation, replacement, kAbsent);
		ExpectDocuments(both, kAbsent, kAbsent);
		Reboot();
	}
}

/* Document setters, commit, and abort outside begin-create/begin-update are rejected and stage nothing. */
ZTEST(aliro_ud_provisioning, test_document_setters_require_open_transaction)
{
	static const char *const kCommands[][2]{
		{ "aliro-ud credential set-document access 1 00", "credential set-document" },
		{ "aliro-ud credential set-document revocation 1 00", "credential set-document" },
		{ "aliro-ud credential clear-document access", "credential clear-document" },
		{ "aliro-ud credential clear-document revocation", "credential clear-document" },
		{ "aliro-ud credential commit", "credential commit" },
		{ "aliro-ud credential abort", "credential abort" },
	};

	for (const auto &command : kCommands) {
		Expect(command[0], ErrToken("NO_TRANSACTION", command[1]));
	}
	zassert_equal(0u, CountCredentials());

	/* A committed transaction is closed as well. */
	const auto handle = Create(0, kAbsent, kAbsent);
	for (const auto &command : kCommands) {
		Expect(command[0], ErrToken("NO_TRANSACTION", command[1]));
	}
	zassert_equal(1u, CountCredentials());
	ExpectDocuments(handle, kAbsent, kAbsent);
}

/*
 * Malformed encoding, wrong declared length or type, zero length, the
 * configured maximum plus one, and duplicate document fields are rejected
 * without changing the staged transaction.
 */
ZTEST(aliro_ud_provisioning, test_malformed_document_input_is_rejected)
{
	static OptionalDocument committed{};
	static OptionalDocument first{};
	committed = MakeDocument(8, 0x21);
	first = MakeDocument(4, 0x31);
	const auto handle = Create(0, committed, kAbsent);
	const std::string oneOver{ std::to_string(kMaxSize + 1) + " " + std::string(2 * (kMaxSize + 1), 'a') };

	Expect("aliro-ud credential begin-update " + HandleText(handle), "OK");
	const std::string rejected[]{
		"other 1 00",	 "Access 1 00",	  "access x 00",   "access -1 00", "access +1 00",
		"access 0x1 00", "access 1.0 00", "access 0 00",   "access 2 00",  "access 1 0000",
		"access 1 0",	 "access 1 zz",	  "access 1 +1",   "access 1 -1",  "access 1 0x",
		"access 1 g0",	 "access " + oneOver,		   "access 99999999999999999999999 00",
	};
	for (const auto &arguments : rejected) {
		Expect("aliro-ud credential set-document " + arguments,
		       ErrToken("INVALID_ARGUMENT", "credential set-document"));
	}
	Expect("aliro-ud credential clear-document other", ErrToken("INVALID_ARGUMENT", "credential clear-document"));
	Expect("aliro-ud credential clear-document ACCESS", ErrToken("INVALID_ARGUMENT", "credential clear-document"));
	Expect("aliro-ud credential commit", "OK handle=" + HandleText(handle));
	ExpectDocuments(handle, committed, kAbsent);

	/* The first setter of each document in a transaction wins; any second one is a duplicate. */
	Expect("aliro-ud credential begin-update " + HandleText(handle), "OK");
	Expect(SetDocument("access", first), "OK");
	Expect(SetDocument("access", committed), ErrToken("DUPLICATE_FIELD", "credential set-document"));
	Expect("aliro-ud credential clear-document access", ErrToken("DUPLICATE_FIELD", "credential clear-document"));
	Expect("aliro-ud credential clear-document revocation", "OK");
	Expect("aliro-ud credential clear-document revocation",
	       ErrToken("DUPLICATE_FIELD", "credential clear-document"));
	Expect(SetDocument("revocation", first), ErrToken("DUPLICATE_FIELD", "credential set-document"));
	Expect("aliro-ud credential commit", "OK handle=" + HandleText(handle));
	ExpectDocuments(handle, first, kAbsent);
}

/* Handles and hex arguments that are not exact decimal/hex text are rejected without side effects. */
ZTEST(aliro_ud_provisioning, test_malformed_handles_and_hex_are_rejected)
{
	static OptionalDocument access{};
	access = MakeDocument(6, 0x55);
	const auto handle = Create(0, access, kAbsent);
	zassert_equal(1u, handle);
	FillMailbox(handle, 0x61);
	Capture(handle, sBefore[0]);

	/* 2^32 + 1 and 2^64 + 1 would wrap to handle 1 if truncated. */
	static const char *const kMalformed[]{ "4294967297", "18446744073709551617", "-1", "+1", "1x", "0x1", "1.0" };
	for (const char *text : kMalformed) {
		Expect(std::string("aliro-ud credential delete ") + text,
		       ErrToken("INVALID_ARGUMENT", "credential delete"));
		Expect(std::string("aliro-ud credential begin-update ") + text,
		       ErrToken("INVALID_ARGUMENT", "credential begin-update"));
		Expect(std::string("aliro-ud document inspect ") + text,
		       ErrToken("INVALID_ARGUMENT", "document inspect"));
		Expect(std::string("aliro-ud document read ") + text + " access 0 1",
		       ErrToken("INVALID_ARGUMENT", "document read"));
		Expect(std::string("aliro-ud mailbox inspect ") + text,
		       ErrToken("INVALID_ARGUMENT", "mailbox inspect"));
		Expect(std::string("aliro-ud mailbox reset ") + text, ErrToken("INVALID_ARGUMENT", "mailbox reset"));
	}
	ExpectState(handle, sBefore[0]);

	/* Well-formed handles of absent credentials fail in the store. */
	Expect("aliro-ud credential delete 0", ErrCode(ALIRO_INVALID_ARGUMENT, "credential delete"));
	Expect("aliro-ud credential delete 2", ErrCode(ALIRO_INVALID_ARGUMENT, "credential delete"));
	Expect("aliro-ud document inspect 2", ErrCode(ALIRO_INVALID_ARGUMENT, "document inspect"));
	Expect("aliro-ud document read 1 other 0 1", ErrToken("INVALID_ARGUMENT", "document read"));
	Expect("aliro-ud document read 1 revocation 0 0", ErrToken("NOT_PRESENT", "document read"));
	Expect("aliro-ud document read 1 access 7 0", ErrToken("INVALID_ARGUMENT", "document read"));
	Expect("aliro-ud document read 1 access 1 18446744073709551615",
	       ErrToken("INVALID_ARGUMENT", "document read"));
	ExpectState(handle, sBefore[0]);

	/* A rejected set-key leaves the previously staged scalar intact. */
	const auto reference = Create(1, kAbsent, kAbsent);
	const std::string corrupt{ std::string(kKeyHex[2]).substr(0, 62) + "zz" };
	Expect("aliro-ud credential begin-update " + HandleText(handle), "OK");
	Expect(std::string("aliro-ud credential set-key ") + kKeyHex[1], "OK");
	Expect("aliro-ud credential set-key " + corrupt, ErrToken("INVALID_ARGUMENT", "credential set-key"));
	Expect("aliro-ud credential set-key +" + std::string(kKeyHex[2]).substr(1),
	       ErrToken("INVALID_ARGUMENT", "credential set-key"));
	Expect("aliro-ud credential commit", "OK handle=" + HandleText(handle));

	static PersistedCredential referenceRecord{};
	zassert_equal(ALIRO_NO_ERROR, CredentialStore::GetFullRecord(reference, referenceRecord));
	zassert_equal(ALIRO_NO_ERROR, CredentialStore::GetFullRecord(handle, sRecord));
	zassert_true(sRecord.mPublicKey == referenceRecord.mPublicKey, "the committed key is the one staged");
	ExpectDocuments(handle, access, kAbsent);
}

/* Payloads the application does not encode are rejected by the adapter before any state changes. */
ZTEST(aliro_ud_provisioning, test_malformed_payload_encoding_is_rejected)
{
	static OptionalDocument access{};
	access = MakeDocument(8, 0x51);
	const auto handle = Create(0, access, kAbsent);
	FillMailbox(handle, 0x71);
	Capture(handle, sBefore[0]);

	static Provisioning::Payload valid{};
	valid = Provisioning::Payload{};
	valid.mHasNewKeyInput = true;
	FromHex(kKeyHex[1], valid.mNewKeyScalar);
	valid.mPolicySet = true;
	valid.mBindingCount = 1;
	FromHex(kGroupHex, valid.mBindings[0].mReaderGroupIdentifier);
	valid.mBindings[0].mTrustType = AliroUd::Credential::TrustType::Direct;
	FromHex(kTrustKeyHex, valid.mBindings[0].mKey);
	zassert_equal(ALIRO_NO_ERROR, UserDeviceStack::Instance().ValidateCredential(Provisioning::AsConstData(valid)));

	static std::array<uint8_t, sizeof(Provisioning::Payload) + 1> bytes{};
	const auto check = [&](ConstData input, AliroError expected) {
		zassert_equal(expected, UserDeviceStack::Instance().ValidateCredential(input));
		CredentialHandle created{ 0x5a };
		zassert_equal(expected, UserDeviceStack::Instance().CreateCredential(input, created));
		zassert_equal(kInvalidCredentialHandle, created);
		zassert_equal(expected, UserDeviceStack::Instance().UpdateCredential(handle, input));
		zassert_equal(1u, CountCredentials());
		ExpectState(handle, sBefore[0]);
	};
	const auto reload = [&]() { std::memcpy(bytes.data(), &valid, sizeof(valid)); };

	reload();
	check(ConstData{ bytes.data(), sizeof(valid) - 1 }, ALIRO_INVALID_DATA_FORMAT);
	check(ConstData{ bytes.data(), sizeof(valid) + 1 }, ALIRO_INVALID_DATA_FORMAT);
	check(ConstData{ bytes.data(), 0 }, ALIRO_INVALID_DATA_FORMAT);
	check(ConstData{ nullptr, sizeof(valid) }, ALIRO_INVALID_DATA_FORMAT);

	bytes[offsetof(Provisioning::Payload, mMagic)] ^= 0x01;
	check(ConstData{ bytes.data(), sizeof(valid) }, ALIRO_INVALID_DATA_FORMAT);
	reload();
	bytes[offsetof(Provisioning::Payload, mVersion)] ^= 0x01;
	check(ConstData{ bytes.data(), sizeof(valid) }, ALIRO_INVALID_DATA_FORMAT);

	for (const size_t offset : Provisioning::kBoolOffsets) {
		for (const uint8_t value : { uint8_t{ 2 }, uint8_t{ 0xff } }) {
			reload();
			bytes[offset] = value;
			check(ConstData{ bytes.data(), sizeof(valid) }, ALIRO_INVALID_DATA_FORMAT);
		}
	}

	/* A well-formed payload with inconsistent document metadata fails validation. */
	static Provisioning::Payload inconsistent{};
	inconsistent = valid;
	inconsistent.mAccessDocument.mPresent = true;
	check(Provisioning::AsConstData(inconsistent), ALIRO_INVALID_ARGUMENT);
}

/* A failed create or update commit changes no credential, document, or mailbox; later commits succeed. */
ZTEST(aliro_ud_provisioning, test_commit_failures_preserve_committed_state)
{
	static OptionalDocument access{};
	static OptionalDocument revocation{};
	static OptionalDocument replacement{};
	access = MakeDocument(8, 0x61);
	revocation = MakeDocument(9, 0x62);
	replacement = MakeDocument(10, 0x63);
	const auto handle = Create(0, access, kAbsent);
	FillMailbox(handle, 0x71);
	Capture(handle, sBefore[0]);

	for (const auto fault : { CredentialTest::FaultPoint::SaveJournal, CredentialTest::FaultPoint::SaveSlot }) {
		BeginCreate(1);
		Expect(SetDocument("access", replacement), "OK");
		Expect(SetDocument("revocation", revocation), "OK");
		CredentialTest::ArmFault(fault);
		Expect("aliro-ud credential commit", ErrCode(ALIRO_ERROR_INTERNAL, "credential commit"));
		zassert_false(CredentialTest::IsFaultArmed(), "the commit must reach persistence");
		ExpectNothingStaged();
		zassert_equal(1u, CountCredentials());

		Expect("aliro-ud credential begin-update " + HandleText(handle), "OK");
		Expect(std::string("aliro-ud credential set-key ") + kKeyHex[2], "OK");
		Expect("aliro-ud credential set-policy 3", "OK");
		Expect("aliro-ud credential clear-document access", "OK");
		Expect(SetDocument("revocation", revocation), "OK");
		CredentialTest::ArmFault(fault);
		Expect("aliro-ud credential commit", ErrCode(ALIRO_ERROR_INTERNAL, "credential commit"));
		zassert_false(CredentialTest::IsFaultArmed(), "the commit must reach persistence");
		ExpectNothingStaged();

		for (int boot = 0; boot < 2; ++boot) {
			zassert_equal(1u, CountCredentials());
			ExpectState(handle, sBefore[0]);
			ExpectDocuments(handle, access, kAbsent);
			Reboot();
		}
	}

	/* A journal erase failing after the committed switch does not undo it. */
	Expect("aliro-ud credential begin-update " + HandleText(handle), "OK");
	Expect(std::string("aliro-ud credential set-key ") + kKeyHex[2], "OK");
	Expect("aliro-ud credential clear-document access", "OK");
	Expect(SetDocument("revocation", revocation), "OK");
	CredentialTest::ArmFault(CredentialTest::FaultPoint::EraseJournal);
	Expect("aliro-ud credential commit", "OK handle=" + HandleText(handle));
	zassert_false(CredentialTest::IsFaultArmed());
	for (int boot = 0; boot < 2; ++boot) {
		ExpectDocuments(handle, kAbsent, revocation);
		zassert_not_equal(sBefore[0].mRecord.mKeyId, sRecord.mKeyId, "the staged key is committed");
		zassert_true(MailboxStore::IsInitialized(handle), "an update keeps the mailbox");
		Reboot();
	}

	const auto created = Create(1, replacement, kAbsent);
	ExpectDocuments(created, replacement, kAbsent);
}

/* Abort and failed commit discard every staged document; a later valid transaction succeeds. */
ZTEST(aliro_ud_provisioning, test_abort_and_failed_commit_leave_nothing_staged)
{
	static OptionalDocument access{};
	static OptionalDocument revocation{};
	access = MakeDocument(12, 0x81);
	revocation = MakeDocument(13, 0x82);

	BeginCreate(0);
	Expect(SetDocument("access", access), "OK");
	Expect(SetDocument("revocation", revocation), "OK");
	Expect("aliro-ud credential abort", "OK");
	ExpectNothingStaged();
	zassert_equal(0u, CountCredentials());

	const auto first = Create(0, kAbsent, kAbsent);
	ExpectDocuments(first, kAbsent, kAbsent);

	/* A create without a key is rejected by the store. */
	Expect("aliro-ud credential begin-create", "OK");
	Expect(std::string("aliro-ud credential set-binding 0 ") + kGroupHex + " direct " + kTrustKeyHex, "OK");
	Expect(SetDocument("access", access), "OK");
	Expect("aliro-ud credential commit", ErrCode(ALIRO_INVALID_ARGUMENT, "credential commit"));
	ExpectNothingStaged();
	zassert_equal(1u, CountCredentials());

	const auto second = Create(1, kAbsent, revocation);
	ExpectDocuments(second, kAbsent, revocation);

	Expect("aliro-ud credential begin-update " + HandleText(second), "OK");
	Expect("aliro-ud credential clear-document revocation", "OK");
	Expect(SetDocument("access", access), "OK");
	Expect("aliro-ud credential abort", "OK");
	ExpectNothingStaged();
	ExpectDocuments(second, kAbsent, revocation);
	ExpectDocuments(first, kAbsent, kAbsent);
}

/* Delete erases the credential's mailbox; any failure keeps the credential, its documents, and its mailbox. */
ZTEST(aliro_ud_provisioning, test_delete_erases_mailbox_and_failures_preserve_state)
{
	static OptionalDocument access{};
	static OptionalDocument revocation{};
	access = MakeDocument(14, 0x91);
	revocation = MakeDocument(15, 0x92);
	const auto target = Create(0, access, revocation);
	const auto other = Create(1, access, kAbsent);
	FillMailbox(target, 0xa1);
	FillMailbox(other, 0xb1);
	Capture(target, sBefore[0]);
	Capture(other, sBefore[1]);

	const auto expectFailedDelete = [&]() {
		Expect("aliro-ud credential delete " + HandleText(target),
		       ErrCode(ALIRO_ERROR_INTERNAL, "credential delete"));
		for (int boot = 0; boot < 2; ++boot) {
			ExpectState(target, sBefore[0]);
			ExpectState(other, sBefore[1]);
			Reboot();
		}
	};

	MailboxTest::ArmMailboxFault(MailboxTest::FaultPoint::EraseSlot);
	expectFailedDelete();

	for (const auto fault : { CredentialTest::FaultPoint::SaveJournal, CredentialTest::FaultPoint::EraseSlot }) {
		CredentialTest::ArmFault(fault);
		expectFailedDelete();
		zassert_false(CredentialTest::IsFaultArmed(), "the delete must reach persistence");
	}

	/* A journal erase failing after the slot erase does not undo the delete. */
	CredentialTest::ArmFault(CredentialTest::FaultPoint::EraseJournal);
	Expect("aliro-ud credential delete " + HandleText(target), "OK");
	zassert_false(CredentialTest::IsFaultArmed());
	for (int boot = 0; boot < 2; ++boot) {
		ExpectDeleted(target);
		ExpectState(other, sBefore[1]);
		Reboot();
	}

	/* The reused handle starts without the deleted credential's mailbox bytes or documents. */
	const auto reused = Create(2, kAbsent, kAbsent);
	zassert_equal(target, reused);
	Expect("aliro-ud mailbox inspect " + HandleText(reused),
	       "OK handle=" + HandleText(reused) +
		       " size=8 readable=1 writable=1 data_subset_configured=0 data_subset_pairs=0 initialized=0 "
		       "has_data=0");
	Expect("aliro-ud mailbox init " + HandleText(reused), "OK");
	Expect("aliro-ud mailbox read " + HandleText(reused) + " 0 8", "OK data=0000000000000000");
	ExpectDocuments(reused, kAbsent, kAbsent);
	ExpectState(other, sBefore[1]);
}

/*
 * Reset erases every credential's mailbox. A credential whose deletion
 * fails keeps its documents and mailbox and the reset reports the failure;
 * a repeated reset completes.
 */
ZTEST(aliro_ud_provisioning, test_reset_erases_mailboxes_and_failures_preserve_state)
{
	static std::array<OptionalDocument, 3> access{};
	std::array<CredentialHandle, 3> handles{};
	for (size_t i = 0; i < handles.size(); ++i) {
		access[i] = MakeDocument(20 + i, static_cast<uint8_t>(0xc0 + i));
		handles[i] = Create(i, access[i], MakeDocument(10 + i, static_cast<uint8_t>(0xd0 + i)));
		FillMailbox(handles[i], static_cast<uint8_t>(0xe0 + 0x10 * i));
	}
	Capture(handles[0], sBefore[0]);

	/* The first mailbox erase is credential 1's. */
	MailboxTest::ArmMailboxFault(MailboxTest::FaultPoint::EraseSlot);
	Expect("aliro-ud credential reset", ErrCode(ALIRO_ERROR_INTERNAL, "credential reset"));
	for (int boot = 0; boot < 2; ++boot) {
		ExpectState(handles[0], sBefore[0]);
		ExpectDeleted(handles[1]);
		ExpectDeleted(handles[2]);
		Reboot();
	}

	for (const auto fault : { CredentialTest::FaultPoint::SaveJournal, CredentialTest::FaultPoint::EraseSlot }) {
		CredentialTest::ArmFault(fault);
		Expect("aliro-ud credential reset", ErrCode(ALIRO_ERROR_INTERNAL, "credential reset"));
		zassert_false(CredentialTest::IsFaultArmed(), "the reset must reach persistence");
		for (int boot = 0; boot < 2; ++boot) {
			ExpectState(handles[0], sBefore[0]);
			Reboot();
		}
	}

	/* Store-wide cleanup failing after every credential is deleted is reported. */
	CredentialTest::ArmFault(CredentialTest::FaultPoint::SavePreferredTable);
	Expect("aliro-ud credential reset", ErrCode(ALIRO_ERROR_INTERNAL, "credential reset"));
	zassert_false(CredentialTest::IsFaultArmed());
	ExpectDeleted(handles[0]);

	Expect("aliro-ud credential reset", "OK");
	for (int boot = 0; boot < 2; ++boot) {
		zassert_equal(0u, CountCredentials());
		for (const auto handle : handles) {
			ExpectDeleted(handle);
		}
		Reboot();
	}
}
