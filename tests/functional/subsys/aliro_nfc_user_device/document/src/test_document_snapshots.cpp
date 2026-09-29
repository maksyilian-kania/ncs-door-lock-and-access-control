/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include <zephyr/ztest.h>

#include "fake_credential_persistence.h"
#include "fake_key_backend.h"
#include "fake_persistent_key_backend.h"
#include "fake_persistent_key_persistence.h"
#include "storage/credential/credential_store.h"
#include "storage/document/document_snapshots.h"
#include "storage/key/persistent_key_store.h"

#include <aliro/user_device/interface.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>

/*
 * Access and Revocation Document storage and snapshots. The User Device
 * SHALL support storage and retrieval of the Access Document (Aliro 1.0
 * Specification, section 7.1, page 32) and of the Revocation Document
 * (section 7.6, page 45); an Access Credential MAY be associated with
 * either (section 6.2, pages 28-29). Document bytes are opaque here:
 * parsing and data-element selection are stack-owned.
 */

using namespace Aliro;
using namespace Aliro::UserDevice;
using ::Aliro::AccessDocumentTypes::DocumentType;
using ::AliroUd::Credential::OptionalDocument;
using ::AliroUd::Credential::PersistedCredential;

namespace Document = ::Aliro::Interface::UserDevice::Document;
namespace Snapshots = ::AliroUd::Document::Snapshots;
namespace CredentialStore = ::AliroUd::Credential::Store;

namespace {

constexpr size_t kMaxSize{ AliroUd::Credential::kDocumentMaxSizeBytes };
constexpr size_t kCredentials{ AliroUd::Credential::kMaxCredentials };
constexpr size_t kSnapshotCapacity{ CONFIG_ALIRO_UD_MAX_OPEN_DOCUMENT_SNAPSHOTS };
constexpr size_t kSizeMax{ std::numeric_limits<size_t>::max() };
constexpr uint8_t kGuard{ 0xa5 };
constexpr std::array<DocumentType, 2> kTypes{ DocumentType::Access, DocumentType::Revocation };

/* Indefinite-length map, non-shortest integer, and unsorted keys: CBOR that deterministic encoding would rewrite. */
constexpr std::array<uint8_t, 10> kNonCanonicalCbor{ 0xbf, 0x61, 0x62, 0x18, 0x01, 0x61, 0x61, 0x00, 0xff, 0x00 };

const OptionalDocument kAbsent{};

AliroUd::Credential::Provisioning::Payload sPayload{};
PersistedCredential sRecord{};
std::array<uint8_t, kMaxSize + 1> sBuffer{};

/* Every snapshot a test opens, closed after the test even if an assertion aborted it. */
std::array<Document::SnapshotHandle, 64> sTracked{};
size_t sTrackedCount{ 0 };

size_t Len(size_t wanted)
{
	return std::min(wanted, kMaxSize);
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

std::array<uint8_t, 32> KeyScalar(uint8_t seed)
{
	std::array<uint8_t, 32> scalar{ 0x23, 0x23, 0x10, 0x22, 0xa3, 0x66, 0x2c, 0xeb, 0x6f, 0x2e, 0x6a,
					0x4e, 0x99, 0x88, 0x66, 0xae, 0x88, 0xd6, 0xe9, 0xda, 0x1c, 0x72,
					0xb0, 0x50, 0xae, 0x5c, 0x20, 0x6a, 0x1d, 0xa4, 0x67, seed };
	return scalar;
}

AliroUd::Credential::Provisioning::Payload &PayloadWith(uint8_t seed, const OptionalDocument &access,
							const OptionalDocument &revocation)
{
	sPayload = AliroUd::Credential::Provisioning::Payload{};
	sPayload.mPolicySet = true;
	sPayload.mPolicy = AuthenticationPolicy::UserDeviceSetting;
	sPayload.mBindingCount = 1;
	sPayload.mBindings[0].mReaderGroupIdentifier.fill(seed);
	sPayload.mBindings[0].mTrustType = AliroUd::Credential::TrustType::Direct;
	sPayload.mBindings[0].mKey[0] = 0x04;
	sPayload.mAccessDocument = access;
	sPayload.mRevocationDocument = revocation;
	return sPayload;
}

CredentialHandle Create(uint8_t seed, const OptionalDocument &access, const OptionalDocument &revocation)
{
	auto &payload = PayloadWith(seed, access, revocation);
	payload.mHasNewKeyInput = true;
	payload.mNewKeyScalar = KeyScalar(seed);

	CredentialHandle handle{ kInvalidCredentialHandle };
	zassert_equal(ALIRO_NO_ERROR, CredentialStore::Create(payload, handle), "credential creation must succeed");
	return handle;
}

void Update(CredentialHandle handle, uint8_t seed, const OptionalDocument &access, const OptionalDocument &revocation)
{
	zassert_equal(ALIRO_NO_ERROR, CredentialStore::Update(handle, PayloadWith(seed, access, revocation)),
		      "credential update must succeed");
}

bool CredentialExists(CredentialHandle handle)
{
	CredentialMetadata metadata{};
	return CredentialStore::GetMetadata(handle, metadata) == ALIRO_NO_ERROR;
}

void ExpectNoOpenSnapshots()
{
	zassert_equal(0u, Snapshots::GetOpenSnapshotCount(), "no snapshot may remain open");
}

/* Boot order from main.cpp; persisted records are re-read, RAM snapshots are gone. */
void Reboot()
{
	ExpectNoOpenSnapshots();
	zassert_equal(ALIRO_NO_ERROR, CredentialStore::Init());
	zassert_equal(ALIRO_NO_ERROR, AliroUd::PersistentKey::Store::Init());
}

/* Replaces a credential's documents on flash, bypassing provisioning validation, then reboots. */
void SeedPersisted(CredentialHandle handle, const OptionalDocument &access, const OptionalDocument &revocation)
{
	zassert_equal(ALIRO_NO_ERROR, CredentialStore::GetFullRecord(handle, sRecord));
	sRecord.mAccessDocument = access;
	sRecord.mRevocationDocument = revocation;
	AliroUd::Credential::Test::WritePersistedSlot(handle - 1, sRecord);
	Reboot();
}

Document::SnapshotHandle OpenOk(CredentialHandle handle, DocumentType type)
{
	Document::SnapshotHandle snapshot{ Document::kInvalidSnapshotHandle };
	zassert_equal(ALIRO_NO_ERROR, Document::Open(handle, type, snapshot), "open must succeed");
	zassert_not_equal(Document::kInvalidSnapshotHandle, snapshot, "an opened snapshot has a valid handle");
	zassert_true(sTrackedCount < sTracked.size());
	sTracked[sTrackedCount++] = snapshot;
	return snapshot;
}

void ExpectOpenFails(CredentialHandle handle, DocumentType type, AliroError expected)
{
	const size_t openBefore = Snapshots::GetOpenSnapshotCount();
	Document::SnapshotHandle snapshot{ 0x5a5a5a5a };
	const auto error = Document::Open(handle, type, snapshot);
	if (error == ALIRO_NO_ERROR && sTrackedCount < sTracked.size()) {
		sTracked[sTrackedCount++] = snapshot;
	}
	zassert_equal(expected, error, "open must fail with the expected error");
	zassert_equal(Document::kInvalidSnapshotHandle, snapshot, "a failed open leaves the handle invalid");
	zassert_equal(openBefore, Snapshots::GetOpenSnapshotCount(), "a failed open holds no snapshot");
}

void ExpectSnapshot(Document::SnapshotHandle snapshot, const OptionalDocument &expected)
{
	size_t size{ kSizeMax };
	zassert_equal(ALIRO_NO_ERROR, Document::GetSize(snapshot, size));
	zassert_equal(expected.mLength, size, "snapshot size must equal the provisioned length");

	sBuffer.fill(kGuard);
	zassert_equal(ALIRO_NO_ERROR, Document::Read(snapshot, 0, sBuffer.data(), size));
	zassert_mem_equal(expected.mData.data(), sBuffer.data(), size,
			  "snapshot bytes must equal the provisioned bytes");
	zassert_equal(kGuard, sBuffer[size], "a read must not write past its length");
}

void ExpectDocument(CredentialHandle handle, DocumentType type, const OptionalDocument &expected)
{
	if (!expected.mPresent) {
		ExpectOpenFails(handle, type, ALIRO_INVALID_ARGUMENT);
		return;
	}

	const auto snapshot = OpenOk(handle, type);
	ExpectSnapshot(snapshot, expected);
	Document::Close(snapshot);
}

void ExpectStored(CredentialHandle handle, const OptionalDocument &access, const OptionalDocument &revocation)
{
	ExpectDocument(handle, DocumentType::Access, access);
	ExpectDocument(handle, DocumentType::Revocation, revocation);
}

bool BufferUntouched()
{
	return std::all_of(sBuffer.begin(), sBuffer.end(), [](uint8_t byte) { return byte == kGuard; });
}

void ResetBeforeEachTest(void *fixture)
{
	(void)fixture;

	sTrackedCount = 0;
	AliroUd::Credential::Test::ResetFakePersistence();
	AliroUd::Credential::Test::ResetFakeKeyBackend();
	AliroUd::PersistentKey::Test::ResetFakePersistentKeyPersistence();
	AliroUd::PersistentKey::Test::ResetFakePersistentKeyBackend();

	zassert_equal(ALIRO_NO_ERROR, CredentialStore::Init());
	zassert_equal(ALIRO_NO_ERROR, AliroUd::PersistentKey::Store::Init());
}

void CloseAfterEachTest(void *fixture)
{
	(void)fixture;

	for (size_t i = 0; i < sTrackedCount; ++i) {
		Document::Close(sTracked[i]);
	}
	sTrackedCount = 0;
}

} // namespace

ZTEST_SUITE(aliro_ud_document, nullptr, nullptr, ResetBeforeEachTest, CloseAfterEachTest, nullptr);

/* The snapshot bound and table capacity satisfy the public Document contract and the two AUTH1 snapshots. */
ZTEST(aliro_ud_document, test_capacity_configuration_matches_public_contract)
{
	zassert_true(kMaxSize <= CONFIG_NCS_ALIRO_USER_DEVICE_MAX_DOCUMENT_SNAPSHOT_BYTES);
	zassert_true(kSnapshotCapacity >= kTypes.size());

	/* Every credential holds both documents at the configured maximum at once. */
	for (size_t i = 0; i < kCredentials; ++i) {
		const auto seed = static_cast<uint8_t>(0xc0 + 2 * i);
		const auto handle = Create(seed, MakeDocument(kMaxSize, seed), MakeDocument(kMaxSize, seed + 1));
		zassert_equal(i + 1, handle);
	}
	for (size_t i = 0; i < kCredentials; ++i) {
		const auto seed = static_cast<uint8_t>(0xc0 + 2 * i);
		ExpectStored(static_cast<CredentialHandle>(i + 1), MakeDocument(kMaxSize, seed),
			     MakeDocument(kMaxSize, seed + 1));
	}

	ExpectNoOpenSnapshots();
}

/* Access and Revocation Documents are created, retrieved, replaced, and deleted independently per credential. */
ZTEST(aliro_ud_document, test_access_and_revocation_are_independent_per_credential)
{
	const size_t count = std::min<size_t>(kCredentials, 3);
	static std::array<OptionalDocument, 3> access{};
	static std::array<OptionalDocument, 3> revocation{};
	std::array<CredentialHandle, 3> handles{};

	/* Both documents, Access only, and Revocation only. */
	for (size_t i = 0; i < count; ++i) {
		access[i] = (i == 2) ? kAbsent : MakeDocument(Len(16 + i), static_cast<uint8_t>(0x10 + i));
		revocation[i] = (i == 1) ? kAbsent : MakeDocument(Len(24 + i), static_cast<uint8_t>(0x40 + i));
		handles[i] = Create(static_cast<uint8_t>(0x20 + i), access[i], revocation[i]);
	}
	for (size_t i = 0; i < count; ++i) {
		ExpectStored(handles[i], access[i], revocation[i]);
	}

	zassert_equal(ALIRO_NO_ERROR, Document::Delete(handles[0], DocumentType::Access));
	access[0] = kAbsent;
	for (size_t i = 0; i < count; ++i) {
		ExpectStored(handles[i], access[i], revocation[i]);
	}
	zassert_equal(ALIRO_NO_ERROR, Document::Delete(handles[0], DocumentType::Access),
		      "deleting an absent document succeeds");

	access[0] = MakeDocument(Len(40), 0x70);
	Update(handles[0], 0x20, access[0], revocation[0]);
	for (size_t i = 0; i < count; ++i) {
		ExpectStored(handles[i], access[i], revocation[i]);
	}

	zassert_equal(ALIRO_NO_ERROR, Document::Delete(handles[0], DocumentType::Revocation));
	revocation[0] = kAbsent;
	for (size_t i = 0; i < count; ++i) {
		ExpectStored(handles[i], access[i], revocation[i]);
	}
	zassert_true(CredentialExists(handles[0]), "document deletion keeps the credential");

	ExpectNoOpenSnapshots();
}

/* Provisioned bytes and document type are returned exactly; nothing is normalized. */
ZTEST(aliro_ud_document, test_document_bytes_and_type_are_preserved_exactly)
{
	static OptionalDocument access{};
	static OptionalDocument revocation{};
	access = MakeDocument(kMaxSize, 0x01);
	std::copy_n(kNonCanonicalCbor.begin(), std::min(kMaxSize, kNonCanonicalCbor.size()), access.mData.begin());
	revocation = MakeDocument(kMaxSize, 0x02);

	const auto handle = Create(0x31, access, revocation);
	ExpectStored(handle, access, revocation);

	zassert_equal(ALIRO_NO_ERROR, CredentialStore::GetFullRecord(handle, sRecord));
	zassert_equal(access.mLength, sRecord.mAccessDocument.mLength);
	zassert_mem_equal(access.mData.data(), sRecord.mAccessDocument.mData.data(), kMaxSize);
	zassert_equal(revocation.mLength, sRecord.mRevocationDocument.mLength);
	zassert_mem_equal(revocation.mData.data(), sRecord.mRevocationDocument.mData.data(), kMaxSize);

	Reboot();
	ExpectStored(handle, access, revocation);

	ExpectNoOpenSnapshots();
}

/* An open snapshot keeps its bytes across replacement and deletion; a new snapshot sees the committed state. */
ZTEST(aliro_ud_document, test_open_snapshot_is_immutable_across_replacement_and_deletion)
{
	static OptionalDocument first{};
	static OptionalDocument firstRevocation{};
	static OptionalDocument second{};
	first = MakeDocument(Len(48), 0x11);
	firstRevocation = MakeDocument(Len(32), 0x22);
	second = MakeDocument(Len(20), 0x33);

	const auto handle = Create(0x41, first, firstRevocation);
	const auto access = OpenOk(handle, DocumentType::Access);
	const auto revocation = OpenOk(handle, DocumentType::Revocation);

	Update(handle, 0x41, second, kAbsent);
	ExpectSnapshot(access, first);
	ExpectSnapshot(revocation, firstRevocation);

	Document::Close(revocation);
	ExpectDocument(handle, DocumentType::Access, second);
	ExpectDocument(handle, DocumentType::Revocation, kAbsent);

	zassert_equal(ALIRO_NO_ERROR, Document::Delete(handle, DocumentType::Access));
	ExpectSnapshot(access, first);
	ExpectDocument(handle, DocumentType::Access, kAbsent);

	zassert_equal(ALIRO_NO_ERROR, CredentialStore::Delete(handle));
	ExpectSnapshot(access, first);
	ExpectDocument(handle, DocumentType::Access, kAbsent);

	Document::Close(access);
	ExpectNoOpenSnapshots();
}

/* A zero-length document opens with size 0 and only an empty read at offset 0 succeeds. */
ZTEST(aliro_ud_document, test_zero_length_document_snapshot)
{
	/* Seeded on flash: provisioning rejects an empty document. */
	OptionalDocument empty{};
	empty.mPresent = true;
	const auto handle = Create(0x51, kAbsent, kAbsent);
	SeedPersisted(handle, empty, empty);

	for (const auto type : kTypes) {
		const auto snapshot = OpenOk(handle, type);
		size_t size{ kSizeMax };
		zassert_equal(ALIRO_NO_ERROR, Document::GetSize(snapshot, size));
		zassert_equal(0u, size);

		zassert_equal(ALIRO_NO_ERROR, Document::Read(snapshot, 0, nullptr, 0));
		sBuffer.fill(kGuard);
		zassert_equal(ALIRO_NO_ERROR, Document::Read(snapshot, 0, sBuffer.data(), 0));
		zassert_equal(ALIRO_INVALID_ARGUMENT, Document::Read(snapshot, 0, sBuffer.data(), 1));
		zassert_equal(ALIRO_INVALID_ARGUMENT, Document::Read(snapshot, 1, sBuffer.data(), 0));
		zassert_equal(ALIRO_INVALID_ARGUMENT, Document::Read(snapshot, kSizeMax, sBuffer.data(), kSizeMax));
		zassert_true(BufferUntouched());
		Document::Close(snapshot);
	}

	ExpectNoOpenSnapshots();
}

/* Reads are bounds-checked without overflow, never write when rejected, and reassemble through a short buffer. */
ZTEST(aliro_ud_document, test_reads_are_overflow_safe)
{
	static OptionalDocument document{};
	document = MakeDocument(kMaxSize, 0x61);
	const auto handle = Create(0x61, document, kAbsent);
	const auto snapshot = OpenOk(handle, DocumentType::Access);

	struct Range {
		size_t mOffset;
		size_t mLength;
	};

	const Range rejected[] = {
		{ 0, kMaxSize + 1 }, { kMaxSize, 1 }, { kMaxSize + 1, 0 },    { 1, kMaxSize },	      { kSizeMax, 0 },
		{ kSizeMax, 1 },     { 1, kSizeMax }, { kMaxSize, kSizeMax }, { kSizeMax, kSizeMax },
	};
	for (const auto &range : rejected) {
		sBuffer.fill(kGuard);
		zassert_equal(ALIRO_INVALID_ARGUMENT,
			      Document::Read(snapshot, range.mOffset, sBuffer.data(), range.mLength),
			      "offset %zu length %zu must be rejected", range.mOffset, range.mLength);
		zassert_true(BufferUntouched(), "a rejected read must not write");
	}

	const Range accepted[] = { { 0, 0 }, { kMaxSize, 0 }, { kMaxSize - 1, 1 }, { 0, kMaxSize } };
	for (const auto &range : accepted) {
		sBuffer.fill(kGuard);
		zassert_equal(ALIRO_NO_ERROR, Document::Read(snapshot, range.mOffset, sBuffer.data(), range.mLength),
			      "offset %zu length %zu must be accepted", range.mOffset, range.mLength);
		zassert_mem_equal(document.mData.data() + range.mOffset, sBuffer.data(), range.mLength);
		zassert_equal(kGuard, sBuffer[range.mLength]);
	}

	/* A destination shorter than the document reassembles it through bounded reads. */
	std::array<uint8_t, 7> chunk{};
	for (size_t offset = 0; offset < kMaxSize; offset += chunk.size()) {
		const size_t length = std::min(chunk.size(), kMaxSize - offset);
		zassert_equal(ALIRO_NO_ERROR, Document::Read(snapshot, offset, chunk.data(), length));
		zassert_mem_equal(document.mData.data() + offset, chunk.data(), length);
	}
	chunk.fill(kGuard);
	zassert_equal(ALIRO_INVALID_ARGUMENT, Document::Read(snapshot, kMaxSize - 1, chunk.data(), 2),
		      "a read crossing the end is rejected, not truncated");
	zassert_true(std::all_of(chunk.begin(), chunk.end(), [](uint8_t byte) { return byte == kGuard; }));

	zassert_equal(ALIRO_INVALID_ARGUMENT, Document::Read(snapshot, 0, nullptr, 1),
		      "a null destination is rejected");

	ExpectSnapshot(snapshot, document);
	Document::Close(snapshot);
	ExpectNoOpenSnapshots();
}

/* The configured maximum is stored and opened; one byte over is never provisioned nor opened from flash. */
ZTEST(aliro_ud_document, test_configured_maximum_and_one_over)
{
	static OptionalDocument access{};
	static OptionalDocument revocation{};
	access = MakeDocument(kMaxSize, 0x71);
	revocation = MakeDocument(kMaxSize, 0x72);
	const auto handle = Create(0x71, access, revocation);

	/* AUTH1 opens both documents of one credential concurrently. */
	const auto accessSnapshot = OpenOk(handle, DocumentType::Access);
	const auto revocationSnapshot = OpenOk(handle, DocumentType::Revocation);
	ExpectSnapshot(accessSnapshot, access);
	ExpectSnapshot(revocationSnapshot, revocation);
	Document::Close(accessSnapshot);
	Document::Close(revocationSnapshot);

	OptionalDocument oneOver{};
	oneOver.mPresent = true;
	oneOver.mLength = static_cast<uint32_t>(kMaxSize + 1);
	zassert_equal(ALIRO_INVALID_ARGUMENT, CredentialStore::Update(handle, PayloadWith(0x71, oneOver, revocation)));
	zassert_equal(ALIRO_INVALID_ARGUMENT, CredentialStore::Update(handle, PayloadWith(0x71, access, oneOver)));
	ExpectStored(handle, access, revocation);

	/* An over-length record on flash fails closed, like the public contract's oversized document. */
	SeedPersisted(handle, oneOver, revocation);
	ExpectOpenFails(handle, DocumentType::Access, ALIRO_NO_MEMORY);
	ExpectDocument(handle, DocumentType::Revocation, revocation);

	OptionalDocument huge{};
	huge.mPresent = true;
	huge.mLength = std::numeric_limits<uint32_t>::max();
	SeedPersisted(handle, access, huge);
	ExpectDocument(handle, DocumentType::Access, access);
	ExpectOpenFails(handle, DocumentType::Revocation, ALIRO_NO_MEMORY);

	ExpectNoOpenSnapshots();
}

/* Invalid credential, document-type, and snapshot handles are rejected without side effects. */
ZTEST(aliro_ud_document, test_invalid_handles_are_rejected)
{
	static OptionalDocument access{};
	static OptionalDocument revocation{};
	access = MakeDocument(Len(12), 0x81);
	revocation = MakeDocument(Len(14), 0x82);
	const auto handle = Create(0x81, access, revocation);
	const auto invalidType = static_cast<DocumentType>(2);

	const CredentialHandle invalidCredentials[] = { kInvalidCredentialHandle,
							static_cast<CredentialHandle>(kCredentials + 1),
							std::numeric_limits<CredentialHandle>::max() };
	for (const auto invalid : invalidCredentials) {
		for (const auto type : kTypes) {
			ExpectOpenFails(invalid, type, ALIRO_INVALID_ARGUMENT);
			zassert_not_equal(ALIRO_NO_ERROR, Document::Delete(invalid, type));
		}
	}
	ExpectOpenFails(handle, invalidType, ALIRO_INVALID_ARGUMENT);
	zassert_equal(ALIRO_INVALID_ARGUMENT, Document::Delete(handle, invalidType));
	ExpectStored(handle, access, revocation);

	if (kCredentials >= 2) {
		const auto bare = Create(0x83, kAbsent, kAbsent);
		ExpectStored(bare, kAbsent, kAbsent);
	}

	const auto closed = OpenOk(handle, DocumentType::Access);
	Document::Close(closed);
	const Document::SnapshotHandle invalidSnapshots[] = { Document::kInvalidSnapshotHandle, closed,
							      std::numeric_limits<Document::SnapshotHandle>::max() };
	for (const auto invalid : invalidSnapshots) {
		size_t size{ 1234 };
		zassert_equal(ALIRO_INVALID_STATE, Document::GetSize(invalid, size));
		zassert_equal(0u, size, "a failed size query reports 0");
		sBuffer.fill(kGuard);
		zassert_equal(ALIRO_INVALID_STATE, Document::Read(invalid, 0, sBuffer.data(), 1));
		zassert_true(BufferUntouched());
		Document::Close(invalid);
	}
	ExpectNoOpenSnapshots();

	/* A closed handle never aliases a later snapshot, and repeated or invalid closes leave it open. */
	const auto newer = OpenOk(handle, DocumentType::Access);
	zassert_not_equal(closed, newer);
	size_t size{ 0 };
	zassert_equal(ALIRO_INVALID_STATE, Document::GetSize(closed, size));
	Document::Close(closed);
	Document::Close(Document::kInvalidSnapshotHandle);
	ExpectSnapshot(newer, access);
	Document::Close(newer);
	Document::Close(newer);
	ExpectNoOpenSnapshots();

	zassert_equal(ALIRO_NO_ERROR, CredentialStore::Delete(handle));
	for (const auto type : kTypes) {
		ExpectOpenFails(handle, type, ALIRO_INVALID_ARGUMENT);
		zassert_not_equal(ALIRO_NO_ERROR, Document::Delete(handle, type));
	}

	ExpectNoOpenSnapshots();
}

/* Snapshot-table exhaustion fails the extra open only; freed slots are reusable. */
ZTEST(aliro_ud_document, test_snapshot_table_exhaustion_and_reuse)
{
	static OptionalDocument access{};
	static OptionalDocument revocation{};
	access = MakeDocument(Len(10), 0x91);
	revocation = MakeDocument(Len(11), 0x92);
	const auto handle = Create(0x91, access, revocation);

	for (int round = 0; round < 2; ++round) {
		std::array<Document::SnapshotHandle, kSnapshotCapacity> open{};
		for (size_t i = 0; i < kSnapshotCapacity; ++i) {
			open[i] = OpenOk(handle, kTypes[i % 2]);
			for (size_t j = 0; j < i; ++j) {
				zassert_not_equal(open[j], open[i], "open snapshot handles are unique");
			}
		}
		zassert_equal(kSnapshotCapacity, Snapshots::GetOpenSnapshotCount());

		for (const auto type : kTypes) {
			ExpectOpenFails(handle, type, ALIRO_NO_MEMORY);
		}
		for (size_t i = 0; i < kSnapshotCapacity; ++i) {
			ExpectSnapshot(open[i], (i % 2 == 0) ? access : revocation);
		}

		Document::Close(open[0]);
		open[0] = OpenOk(handle, DocumentType::Access);
		ExpectOpenFails(handle, DocumentType::Access, ALIRO_NO_MEMORY);

		for (const auto snapshot : open) {
			Document::Close(snapshot);
		}
		ExpectNoOpenSnapshots();
	}
}

/* Committed documents survive reboots; deleted documents and credentials do not reappear. */
ZTEST(aliro_ud_document, test_committed_documents_survive_reboot)
{
	const size_t count = std::min<size_t>(kCredentials, 3);
	static std::array<OptionalDocument, 3> access{};
	static std::array<OptionalDocument, 3> revocation{};
	std::array<CredentialHandle, 3> handles{};

	for (size_t i = 0; i < count; ++i) {
		access[i] = MakeDocument(Len(30 + i), static_cast<uint8_t>(0xa0 + i));
		revocation[i] = MakeDocument(Len(20 + i), static_cast<uint8_t>(0xa8 + i));
		handles[i] = Create(static_cast<uint8_t>(0xa0 + i), access[i], revocation[i]);
	}
	Reboot();
	for (size_t i = 0; i < count; ++i) {
		ExpectStored(handles[i], access[i], revocation[i]);
	}

	zassert_equal(ALIRO_NO_ERROR, Document::Delete(handles[0], DocumentType::Access));
	access[0] = kAbsent;
	if (count >= 2) {
		zassert_equal(ALIRO_NO_ERROR, Document::Delete(handles[1], DocumentType::Revocation));
		revocation[1] = kAbsent;
	}
	if (count >= 3) {
		zassert_equal(ALIRO_NO_ERROR, CredentialStore::Delete(handles[2]));
	}

	for (int boot = 0; boot < 2; ++boot) {
		Reboot();
		for (size_t i = 0; i < std::min<size_t>(count, 2); ++i) {
			ExpectStored(handles[i], access[i], revocation[i]);
		}
		if (count >= 3) {
			zassert_false(CredentialExists(handles[2]));
			ExpectStored(handles[2], kAbsent, kAbsent);
		}
	}

	ExpectNoOpenSnapshots();
}

/* Document and credential factory resets survive reboots; later provisioning is retained. */
ZTEST(aliro_ud_document, test_reset_documents_do_not_reappear)
{
	const size_t count = std::min<size_t>(kCredentials, 3);
	std::array<CredentialHandle, 3> handles{};
	for (size_t i = 0; i < count; ++i) {
		handles[i] = Create(static_cast<uint8_t>(0xb0 + i), MakeDocument(Len(8 + i), static_cast<uint8_t>(i)),
				    MakeDocument(Len(9 + i), static_cast<uint8_t>(0x80 + i)));
	}

	zassert_equal(ALIRO_NO_ERROR, Document::Reset());
	for (int boot = 0; boot < 2; ++boot) {
		for (size_t i = 0; i < count; ++i) {
			zassert_true(CredentialExists(handles[i]), "a document reset keeps the credential");
			ExpectStored(handles[i], kAbsent, kAbsent);
		}
		Reboot();
	}

	static OptionalDocument access{};
	access = MakeDocument(Len(9), 0xb8);
	Update(handles[0], 0xb0, access, kAbsent);
	Reboot();
	ExpectStored(handles[0], access, kAbsent);

	zassert_equal(ALIRO_NO_ERROR, CredentialStore::Reset());
	for (int boot = 0; boot < 2; ++boot) {
		for (size_t i = 0; i < count; ++i) {
			zassert_false(CredentialExists(handles[i]));
			ExpectStored(handles[i], kAbsent, kAbsent);
		}
		Reboot();
	}

	ExpectNoOpenSnapshots();
}

/*
 * Present documents are 1 to the configured maximum bytes long (a document
 * is a non-empty CBOR structure, section 7.2, page 32); absent documents and
 * bytes past the length are zero. Anything else is rejected before any
 * state changes.
 */
ZTEST(aliro_ud_document, test_provisioning_rejects_inconsistent_documents)
{
	static OptionalDocument committed{};
	committed = MakeDocument(Len(5), 0x13);
	const auto handle = Create(0x13, committed, kAbsent);

	static std::array<OptionalDocument, 5> rejected{};
	size_t count = 0;
	rejected[count] = OptionalDocument{};
	rejected[count++].mPresent = true;
	rejected[count] = OptionalDocument{};
	rejected[count].mPresent = true;
	rejected[count++].mLength = static_cast<uint32_t>(kMaxSize + 1);
	rejected[count] = OptionalDocument{};
	rejected[count++].mLength = 1;
	rejected[count] = OptionalDocument{};
	rejected[count++].mData[kMaxSize - 1] = 0x01;
	if (kMaxSize >= 2) {
		rejected[count] = MakeDocument(kMaxSize - 1, 0x21);
		rejected[count++].mData[kMaxSize - 1] = 0x01;
	}

	for (size_t i = 0; i < count; ++i) {
		for (const bool asRevocation : { false, true }) {
			const auto &access = asRevocation ? kAbsent : rejected[i];
			const auto &revocation = asRevocation ? rejected[i] : kAbsent;

			auto &payload = PayloadWith(0x14, access, revocation);
			zassert_equal(ALIRO_INVALID_ARGUMENT, CredentialStore::Validate(payload), "case %zu", i);
			payload.mHasNewKeyInput = true;
			payload.mNewKeyScalar = KeyScalar(0x14);
			CredentialHandle created{ 0x5a };
			zassert_equal(ALIRO_INVALID_ARGUMENT, CredentialStore::Create(payload, created), "case %zu", i);
			zassert_equal(kInvalidCredentialHandle, created);
			zassert_equal(ALIRO_INVALID_ARGUMENT,
				      CredentialStore::Update(handle, PayloadWith(0x13, access, revocation)), "case %zu",
				      i);
			ExpectStored(handle, committed, kAbsent);
			if (kCredentials >= 2) {
				zassert_false(CredentialExists(handle + 1));
			}
		}
	}
	Reboot();
	ExpectStored(handle, committed, kAbsent);

	/* The smallest and largest documents are accepted. */
	static OptionalDocument smallest{};
	static OptionalDocument largest{};
	smallest = MakeDocument(1, 0x15);
	largest = MakeDocument(kMaxSize, 0x16);
	Update(handle, 0x13, smallest, largest);
	ExpectStored(handle, smallest, largest);

	ExpectNoOpenSnapshots();
}

/* A failed document clear keeps the committed document in RAM and on flash; deleting nothing writes nothing. */
ZTEST(aliro_ud_document, test_failed_document_clear_keeps_committed_document)
{
	namespace FakeFlash = AliroUd::Credential::Test;

	static OptionalDocument access{};
	static OptionalDocument revocation{};
	access = MakeDocument(Len(12), 0xe1);
	revocation = MakeDocument(Len(13), 0xe2);
	const auto handle = Create(0xe1, access, revocation);

	for (const auto type : kTypes) {
		FakeFlash::ArmFault(FakeFlash::FaultPoint::SaveSlot);
		zassert_equal(ALIRO_ERROR_INTERNAL, Document::Delete(handle, type));
		zassert_false(FakeFlash::IsFaultArmed(), "the delete must reach persistence");
		for (int boot = 0; boot < 2; ++boot) {
			ExpectStored(handle, access, revocation);
			Reboot();
		}
	}

	/* A document reset keeps the document whose deletion failed and deletes the others. */
	FakeFlash::ArmFault(FakeFlash::FaultPoint::SaveSlot);
	zassert_equal(ALIRO_ERROR_INTERNAL, Document::Reset());
	for (int boot = 0; boot < 2; ++boot) {
		ExpectStored(handle, access, kAbsent);
		Reboot();
	}

	FakeFlash::ArmFault(FakeFlash::FaultPoint::SaveSlot);
	zassert_equal(ALIRO_NO_ERROR, Document::Delete(handle, DocumentType::Revocation));
	zassert_true(FakeFlash::IsFaultArmed(), "deleting an absent document writes nothing");
	FakeFlash::ArmFault(FakeFlash::FaultPoint::None);

	zassert_equal(ALIRO_NO_ERROR, Document::Delete(handle, DocumentType::Access));
	for (int boot = 0; boot < 2; ++boot) {
		zassert_true(CredentialExists(handle));
		ExpectStored(handle, kAbsent, kAbsent);
		Reboot();
	}

	ExpectNoOpenSnapshots();
}

/* A failed credential delete keeps its documents across reboot; a completed one survives a failed journal erase. */
ZTEST(aliro_ud_document, test_failed_credential_delete_keeps_documents)
{
	namespace FakeFlash = AliroUd::Credential::Test;

	static OptionalDocument access{};
	static OptionalDocument revocation{};
	access = MakeDocument(Len(17), 0xf1);
	revocation = MakeDocument(Len(18), 0xf2);
	const auto handle = Create(0xf1, access, revocation);

	for (const auto fault : { FakeFlash::FaultPoint::SaveJournal, FakeFlash::FaultPoint::EraseSlot }) {
		FakeFlash::ArmFault(fault);
		zassert_equal(ALIRO_ERROR_INTERNAL, CredentialStore::Delete(handle));
		zassert_false(FakeFlash::IsFaultArmed(), "the delete must reach persistence");
		for (int boot = 0; boot < 2; ++boot) {
			zassert_true(CredentialExists(handle), "a failed delete is not finished at boot");
			ExpectStored(handle, access, revocation);
			Reboot();
		}
	}

	FakeFlash::ArmFault(FakeFlash::FaultPoint::EraseJournal);
	zassert_equal(ALIRO_NO_ERROR, CredentialStore::Delete(handle));
	zassert_false(FakeFlash::IsFaultArmed());
	for (int boot = 0; boot < 2; ++boot) {
		zassert_false(CredentialExists(handle));
		ExpectStored(handle, kAbsent, kAbsent);
		Reboot();
	}

	const auto reused = Create(0xf3, revocation, kAbsent);
	zassert_equal(handle, reused);
	for (int boot = 0; boot < 2; ++boot) {
		ExpectStored(reused, revocation, kAbsent);
		Reboot();
	}

	ExpectNoOpenSnapshots();
}

/* Snapshots are released by Close() after success, failed calls, and resets, leaving full capacity. */
ZTEST(aliro_ud_document, test_snapshots_are_released_on_success_failure_and_reset)
{
	static OptionalDocument access{};
	static OptionalDocument revocation{};
	access = MakeDocument(Len(15), 0xd1);
	revocation = MakeDocument(Len(16), 0xd2);
	auto handle = Create(0xd1, access, revocation);

	ExpectDocument(handle, DocumentType::Access, access);
	ExpectNoOpenSnapshots();

	/* Failed reads and size queries leave the snapshot open until Close(). */
	{
		const auto snapshot = OpenOk(handle, DocumentType::Revocation);
		zassert_equal(ALIRO_INVALID_ARGUMENT,
			      Document::Read(snapshot, revocation.mLength + 1, sBuffer.data(), 0));
		zassert_equal(1u, Snapshots::GetOpenSnapshotCount());
		ExpectSnapshot(snapshot, revocation);
		Document::Close(snapshot);
		size_t size{ 0 };
		zassert_equal(ALIRO_INVALID_STATE, Document::GetSize(snapshot, size));
	}
	ExpectNoOpenSnapshots();

	/* A document reset keeps open snapshots readable until Close(). */
	{
		const auto accessSnapshot = OpenOk(handle, DocumentType::Access);
		const auto revocationSnapshot = OpenOk(handle, DocumentType::Revocation);
		zassert_equal(ALIRO_NO_ERROR, Document::Reset());
		ExpectStored(handle, kAbsent, kAbsent);
		ExpectSnapshot(accessSnapshot, access);
		ExpectSnapshot(revocationSnapshot, revocation);
		Document::Close(accessSnapshot);
		Document::Close(revocationSnapshot);
	}
	ExpectNoOpenSnapshots();

	/* Every slot is reusable afterwards. */
	Update(handle, 0xd1, access, revocation);
	{
		std::array<Document::SnapshotHandle, kSnapshotCapacity> open{};
		for (size_t i = 0; i < kSnapshotCapacity; ++i) {
			open[i] = OpenOk(handle, kTypes[i % 2]);
		}
		for (const auto snapshot : open) {
			Document::Close(snapshot);
		}
	}
	ExpectNoOpenSnapshots();

	/* A credential factory reset likewise. */
	{
		const auto snapshot = OpenOk(handle, DocumentType::Access);
		zassert_equal(ALIRO_NO_ERROR, CredentialStore::Reset());
		ExpectStored(handle, kAbsent, kAbsent);
		ExpectSnapshot(snapshot, access);
		Document::Close(snapshot);
	}
	ExpectNoOpenSnapshots();

	/* A failed document delete leaves an open snapshot intact and releasable. */
	handle = Create(0xd3, access, revocation);
	{
		const auto snapshot = OpenOk(handle, DocumentType::Access);
		AliroUd::Credential::Test::ArmFault(AliroUd::Credential::Test::FaultPoint::SaveSlot);
		zassert_not_equal(ALIRO_NO_ERROR, Document::Delete(handle, DocumentType::Access));
		zassert_false(AliroUd::Credential::Test::IsFaultArmed(), "the delete must reach persistence");
		ExpectSnapshot(snapshot, access);
		Document::Close(snapshot);
	}
	ExpectNoOpenSnapshots();
}
