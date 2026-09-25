/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include <zephyr/ztest.h>

#include "fake_credential_persistence.h"
#include "fake_key_backend.h"
#include "fake_psa_key_backend.h"
#include "fake_record_persistence.h"
#include "recovery_support.h"
#include "storage/credential/credential_store.h"
#include "storage/key/persistent_key_store.h"

#include <algorithm>
#include <array>

/*
 * Credential deletion and factory reset remove the credential's Kpersistent
 * records and report a failure to do so. Kpersistent is bound to its
 * Access Credential (Aliro 1.0 Specification, section 8.3.1.13, page 59),
 * and lifecycle management is implementation-specific (section 6.2,
 * page 28). A missing Kpersistent is a supported protocol state (section
 * 8.3.3.2.6, pages 69-70), so records are removed before the credential.
 */

using namespace Aliro;
using namespace Aliro::UserDevice;
using namespace AliroUd::PersistentKey::RecoverySupport;

namespace CredentialStore = ::AliroUd::Credential::Store;
namespace FakePsa = ::AliroUd::PersistentKey::FakePsa;
namespace FakeStorage = ::AliroUd::PersistentKey::FakeStorage;

using FakePsa::Kind;

namespace {

constexpr size_t kPerCredential{ AliroUd::PersistentKey::kMaxRecordsPerCredential };
constexpr size_t kGlobal{ AliroUd::PersistentKey::kMaxRecords };
constexpr size_t kCredentials{ CONFIG_ALIRO_UD_MAX_CREDENTIALS };

std::array<uint8_t, 32> KeyScalar(uint8_t seed)
{
	std::array<uint8_t, 32> scalar{ 0x23, 0x23, 0x10, 0x22, 0xa3, 0x66, 0x2c, 0xeb, 0x6f, 0x2e, 0x6a,
					0x4e, 0x99, 0x88, 0x66, 0xae, 0x88, 0xd6, 0xe9, 0xda, 0x1c, 0x72,
					0xb0, 0x50, 0xae, 0x5c, 0x20, 0x6a, 0x1d, 0xa4, 0x67, seed };
	return scalar;
}

CredentialHandle CreateCredential(uint8_t seed)
{
	ReaderGroupIdentifier readerGroupId{};
	readerGroupId.fill(seed);
	CryptoTypes::PublicKey trustAnchor{};
	trustAnchor[0] = 0x04;

	AliroUd::Credential::Provisioning::Payload payload{};
	payload.mHasNewKeyInput = true;
	payload.mNewKeyScalar = KeyScalar(seed);
	payload.mPolicySet = true;
	payload.mPolicy = AuthenticationPolicy::UserDeviceSetting;
	payload.mBindingCount = 1;
	payload.mBindings[0].mReaderGroupIdentifier = readerGroupId;
	payload.mBindings[0].mTrustType = AliroUd::Credential::TrustType::Direct;
	payload.mBindings[0].mKey = trustAnchor;

	CredentialHandle handle{ kInvalidCredentialHandle };
	zassert_equal(ALIRO_NO_ERROR, CredentialStore::Create(payload, handle), "credential creation must succeed");
	return handle;
}

bool CredentialExists(CredentialHandle handle)
{
	CredentialMetadata metadata{};
	return CredentialStore::GetMetadata(handle, metadata) == ALIRO_NO_ERROR;
}

void ResetBeforeEachTest(void *fixture)
{
	(void)fixture;

	ClearAll();
	AliroUd::Credential::Test::ResetFakePersistence();
	AliroUd::Credential::Test::ResetFakeKeyBackend();

	/* Boot order from main.cpp. */
	zassert_equal(ALIRO_NO_ERROR, CredentialStore::Init());
	zassert_equal(ALIRO_NO_ERROR, AliroUd::PersistentKey::Store::Init());
}

} // namespace

ZTEST_SUITE(aliro_ud_persistent_key_credential_cascade, nullptr, nullptr, ResetBeforeEachTest, nullptr, nullptr);

/* Deleting a credential removes exactly its records, and a reused handle starts without any. */
ZTEST(aliro_ud_persistent_key_credential_cascade, test_credential_delete_removes_its_records)
{
	const CredentialHandle credential = CreateCredential(0x31);
	const size_t ownCount = std::min<size_t>(kPerCredential, 2);
	for (size_t i = 0; i < ownCount; ++i) {
		(void)Insert(credential, MakeSub(static_cast<uint8_t>(0x10 + i)), static_cast<uint8_t>(0x20 + i));
	}

	const bool hasOther = kCredentials >= 2;
	CredentialHandle otherCredential{ kInvalidCredentialHandle };
	Stored other{};
	if (hasOther) {
		otherCredential = CreateCredential(0x32);
		other = Insert(otherCredential, MakeSub(0x18), 0x28);
	}

	zassert_equal(ALIRO_NO_ERROR, CredentialStore::Delete(credential));
	zassert_false(CredentialExists(credential));
	for (size_t i = 0; i < ownCount; ++i) {
		ExpectMiss(credential, MakeSub(static_cast<uint8_t>(0x10 + i)));
	}
	zassert_equal(hasOther ? 1u : 0u, FakePsa::LiveCount(Kind::Durable), "the credential's keys are destroyed");
	if (hasOther) {
		ExpectResolves(otherCredential, MakeSub(0x18), other);
	}

	const CredentialHandle reused = CreateCredential(0x33);
	zassert_equal(credential, reused, "the freed handle is reused");
	ExpectMiss(reused, MakeSub(0x10));
	ExpectConsistent();

	ExpectCleanTeardown();
}

/* A record-cleanup failure is returned and leaves the credential committed; a retry completes. */
ZTEST(aliro_ud_persistent_key_credential_cascade, test_credential_delete_propagates_record_cleanup_failure)
{
	const CredentialHandle credential = CreateCredential(0x41);
	const ReaderGroupSubIdentifier sub = MakeSub(0x42);
	const Stored stored = Insert(credential, sub, 0x43);

	FakeStorage::FailNextErase();
	zassert_not_equal(ALIRO_NO_ERROR, CredentialStore::Delete(credential), "the erase failure must be returned");
	zassert_true(CredentialExists(credential), "the credential must stay committed");
	ExpectResolves(credential, sub, stored);

	FakePsa::FailNext(FakePsa::Fault::Destroy);
	zassert_not_equal(ALIRO_NO_ERROR, CredentialStore::Delete(credential), "the destroy failure must be returned");
	zassert_true(CredentialExists(credential), "the credential must stay committed");
	ExpectMiss(credential, sub);
	zassert_equal(1u, FakePsa::LiveCount(Kind::Durable), "the key is retained for recovery");

	zassert_equal(ALIRO_NO_ERROR, CredentialStore::Delete(credential), "a retry must succeed");
	zassert_false(CredentialExists(credential));

	/* The retained key is swept at boot. */
	zassert_equal(ALIRO_NO_ERROR, Reboot());
	ExpectConsistent();
	zassert_equal(0u, FakePsa::LiveCount(Kind::Durable));

	ExpectCleanTeardown();
}

/* Factory reset removes every record, including one whose credential no longer exists. */
ZTEST(aliro_ud_persistent_key_credential_cascade, test_credential_reset_removes_all_records)
{
	const CredentialHandle credential = CreateCredential(0x51);
	(void)Insert(credential, MakeSub(0x52), 0x53);
	if (kGlobal >= 2) {
		/* Left by earlier firmware, which ignored cleanup failures. */
		(void)Insert(static_cast<CredentialHandle>(kCredentials + 1), MakeSub(0x54), 0x55);
	}

	zassert_equal(ALIRO_NO_ERROR, CredentialStore::Reset());
	zassert_false(CredentialExists(credential));
	zassert_equal(0u, FakeStorage::PresentCount());
	zassert_equal(0u, FakePsa::LiveCount(Kind::Durable));

	ExpectCleanTeardown();
}

/* A record-cleanup failure during factory reset is returned, and a repeated reset completes. */
ZTEST(aliro_ud_persistent_key_credential_cascade, test_credential_reset_propagates_record_cleanup_failure)
{
	const CredentialHandle credential = CreateCredential(0x61);
	(void)Insert(credential, MakeSub(0x62), 0x63);

	FakePsa::FailNext(FakePsa::Fault::Destroy);
	zassert_not_equal(ALIRO_NO_ERROR, CredentialStore::Reset(), "the destroy failure must be returned");
	zassert_equal(0u, FakeStorage::PresentCount());
	zassert_equal(1u, FakePsa::LiveCount(Kind::Durable), "the key is retained for recovery");

	zassert_equal(ALIRO_NO_ERROR, CredentialStore::Reset(), "a repeated reset must succeed");
	zassert_false(CredentialExists(credential));
	zassert_equal(0u, FakePsa::LiveCount(Kind::Durable));

	ExpectCleanTeardown();
}
