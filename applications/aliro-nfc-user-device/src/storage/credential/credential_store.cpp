/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include "credential_store.h"

#include "credential_persistence.h"
#include "key_backend.h"

#include <aliro/utils.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(aliro_ud_credential, CONFIG_ALIRO_UD_CREDENTIAL_LOG_LEVEL);

using namespace Aliro;
using namespace Aliro::UserDevice;

/*
 * Credential/trust persistence engine (APP_PLAN.md AWP3). See
 * credential_store.h for the module boundary and lifecycle-coordinator
 * calling requirement.
 *
 * Key-ID allocation: slot i (0-based, handle = i + 1) owns exactly two
 * persistent PSA key IDs: SlotCommittedKeyId(i) is never written to disk
 * directly by this module (the actually-committed key ID is whatever value
 * is stored in the slot's PersistedCredential::mKeyId - it starts as
 * SlotCommittedKeyId(i) for a freshly created credential, but a rotated
 * credential's committed key can end up holding what was previously its
 * "staged" ID, and vice versa on the next rotation - the two IDs simply
 * ping-pong between "committed" and "staged" across successive updates so
 * that a replacement key can always be imported at a *different* ID than
 * the one currently committed, which is what makes the create-new-then-
 * retire-old journal sequence possible without ever colliding IDs).
 */
namespace AliroUd::Credential::Store {
namespace {

K_MUTEX_DEFINE(sMutex);

std::array<PersistedCredential, kMaxCredentials> sSlots{};
PreferredTable sPreferred{};

class Lock {
public:
	Lock() { k_mutex_lock(&sMutex, K_FOREVER); }
	~Lock() { k_mutex_unlock(&sMutex); }
	Lock(const Lock &) = delete;
	Lock &operator=(const Lock &) = delete;
};

constexpr CryptoTypes::KeyId kKeyIdBase{ CONFIG_ALIRO_UD_CREDENTIAL_KEY_ID_BASE };

constexpr CryptoTypes::KeyId SlotPrimaryKeyId(size_t slotIndex)
{
	return kKeyIdBase + static_cast<CryptoTypes::KeyId>(slotIndex) * 2;
}

constexpr CryptoTypes::KeyId SlotAlternateKeyId(size_t slotIndex)
{
	return kKeyIdBase + static_cast<CryptoTypes::KeyId>(slotIndex) * 2 + 1;
}

/** @brief Scratch key ID reserved for transient scalar validation in `Validate()`. */
constexpr CryptoTypes::KeyId kScratchValidationKeyId{ kKeyIdBase +
							static_cast<CryptoTypes::KeyId>(kMaxCredentials) * 2 };

/** @brief The slot's other key ID (whichever of the two is not `currentKeyId`). */
CryptoTypes::KeyId OtherSlotKeyId(size_t slotIndex, CryptoTypes::KeyId currentKeyId)
{
	const auto primary = SlotPrimaryKeyId(slotIndex);
	const auto alternate = SlotAlternateKeyId(slotIndex);
	return (currentKeyId == primary) ? alternate : primary;
}

bool HandleToSlotIndex(CredentialHandle handle, size_t &outIndex)
{
	if (handle == kInvalidCredentialHandle || handle > kMaxCredentials) {
		return false;
	}

	outIndex = static_cast<size_t>(handle - 1);
	return true;
}

CredentialHandle SlotIndexToHandle(size_t slotIndex)
{
	return static_cast<CredentialHandle>(slotIndex + 1);
}

bool ReaderGroupIdentifierEquals(const ReaderGroupIdentifier &a, const ReaderGroupIdentifier &b)
{
	return a == b;
}

/*
 * Validates payload structure/bounds and (if a new key is staged) the raw
 * scalar's cryptographic validity, via a transient PSA import immediately
 * destroyed again - no persistent state changes result either way.
 */
AliroError ValidatePayloadShape(const Provisioning::Payload &input)
{
	if (input.mBindingCount > kMaxBindingsPerCredential) {
		LOG_WRN("Binding count %u exceeds capacity %zu", input.mBindingCount, kMaxBindingsPerCredential);
		return ALIRO_INVALID_ARGUMENT;
	}

	for (uint32_t i = 0; i < input.mBindingCount; ++i) {
		if (input.mBindings[i].mTrustType == TrustType::None) {
			LOG_WRN("Binding %u has no trust type", i);
			return ALIRO_INVALID_ARGUMENT;
		}

		for (uint32_t j = 0; j < i; ++j) {
			if (input.mBindings[j].mTrustType == input.mBindings[i].mTrustType &&
			    ReaderGroupIdentifierEquals(input.mBindings[j].mReaderGroupIdentifier,
							input.mBindings[i].mReaderGroupIdentifier)) {
				LOG_WRN("Duplicate {reader_group_identifier, trust_type} at bindings %u/%u", j, i);
				return ALIRO_INVALID_ARGUMENT;
			}
		}
	}

	if (input.mMailbox.mConfigured && input.mMailbox.mSizeBytes > kMailboxMaxSizeBytes) {
		LOG_WRN("Mailbox size %u exceeds capacity %zu", input.mMailbox.mSizeBytes, kMailboxMaxSizeBytes);
		return ALIRO_INVALID_ARGUMENT;
	}

	if (input.mAccessDocument.mPresent && input.mAccessDocument.mLength > kDocumentMaxSizeBytes) {
		return ALIRO_INVALID_ARGUMENT;
	}

	if (input.mRevocationDocument.mPresent && input.mRevocationDocument.mLength > kDocumentMaxSizeBytes) {
		return ALIRO_INVALID_ARGUMENT;
	}

	switch (input.mPolicy) {
	case AuthenticationPolicy::UserDeviceSetting:
	case AuthenticationPolicy::UserDeviceSettingSecureAction:
	case AuthenticationPolicy::ForceUserAuthentication:
		break;
	default:
		return ALIRO_INVALID_ARGUMENT;
	}

	if (input.mHasNewKeyInput) {
		CryptoTypes::PublicKey scratchPublicKey{};
		const auto importError =
			KeyBackend::ImportPrivateKeyScalar(kScratchValidationKeyId, input.mNewKeyScalar, scratchPublicKey);
		/* Always clean up the scratch slot, whether or not import succeeded. */
		KeyBackend::DestroyKey(kScratchValidationKeyId);

		if (importError != ALIRO_NO_ERROR) {
			return ALIRO_INVALID_ARGUMENT;
		}
	}

	return ALIRO_NO_ERROR;
}

void ApplyPayloadToRecord(const Provisioning::Payload &input, PersistedCredential &record)
{
	record.mPolicy = input.mPolicySet ? input.mPolicy : record.mPolicy;
	record.mBindingCount = input.mBindingCount;
	record.mBindings = input.mBindings;
	record.mMailbox = input.mMailbox;
	record.mHasCredentialSignedTimestamp = input.mHasCredentialSignedTimestamp;
	record.mCredentialSignedTimestamp = input.mCredentialSignedTimestamp;
	record.mHasRevocationSignedTimestamp = input.mHasRevocationSignedTimestamp;
	record.mRevocationSignedTimestamp = input.mRevocationSignedTimestamp;
	record.mAccessDocument = input.mAccessDocument;
	record.mRevocationDocument = input.mRevocationDocument;
}

/* Removes every preferred-table entry pointing at `handle`; persists only if changed. */
AliroError PurgePreferredEntriesForHandle(CredentialHandle handle)
{
	bool changed = false;

	for (auto &entry : sPreferred.mEntries) {
		if (entry.mValid && entry.mPreferredHandle == handle) {
			entry = PreferredBinding{};
			changed = true;
		}
	}

	if (!changed) {
		return ALIRO_NO_ERROR;
	}

	return Persistence::SavePreferredTable(sPreferred);
}

/*
 * Deletes one slot's committed record and destroys its key, through the
 * journal (crash-safe: see the JournalOp::Delete recovery case in Init()).
 */
AliroError DeleteSlotInternal(size_t slotIndex)
{
	if (!sSlots[slotIndex].mValid) {
		return ALIRO_NO_ERROR;
	}

	const CredentialHandle handle = SlotIndexToHandle(slotIndex);
	const CryptoTypes::KeyId oldKeyId = sSlots[slotIndex].mKeyId;

	JournalRecord journal{};
	journal.mOp = JournalOp::Delete;
	journal.mHandle = handle;
	journal.mStagedKeyId = 0;
	journal.mOldKeyId = oldKeyId;

	auto error = Persistence::SaveJournal(journal);
	VerifyOrReturnStatus(error == ALIRO_NO_ERROR, error, LOG_ERR("Failed to journal delete"));

	error = Persistence::EraseSlot(slotIndex);
	VerifyOrReturnStatus(error == ALIRO_NO_ERROR, error, LOG_ERR("Failed to erase credential slot"));

	sSlots[slotIndex] = PersistedCredential{};

	KeyBackend::DestroyKey(oldKeyId);
	PurgePreferredEntriesForHandle(handle);

	return Persistence::EraseJournal();
}

/*
 * Runs the create/update transaction for `slotIndex`: import a replacement
 * key if one is staged (leaving the previously-committed key, if any,
 * untouched until the metadata switch below has durably landed), journal
 * the intent, atomically switch the committed metadata record, then retire
 * the previous key. See credential_store.h and the AWP3 evidence entry for
 * the crash-recovery argument.
 */
AliroError CommitTransaction(size_t slotIndex, const Provisioning::Payload &input, bool isUpdate,
			     CredentialHandle &outHandle)
{
	const CredentialHandle handle = SlotIndexToHandle(slotIndex);
	const PersistedCredential &existing = sSlots[slotIndex];
	const CryptoTypes::KeyId oldKeyId = isUpdate ? existing.mKeyId : 0;

	CryptoTypes::KeyId stagedKeyId{ 0 };
	CryptoTypes::PublicKey newPublicKey{};

	if (input.mHasNewKeyInput) {
		stagedKeyId = OtherSlotKeyId(slotIndex, oldKeyId);
		/* Self-heal: destroy any orphaned leftover at the staged ID before reusing it. */
		KeyBackend::DestroyKey(stagedKeyId);

		const auto importError = KeyBackend::ImportPrivateKeyScalar(stagedKeyId, input.mNewKeyScalar, newPublicKey);
		if (importError != ALIRO_NO_ERROR) {
			return importError;
		}
	} else {
		if (!isUpdate) {
			/* Create() already enforces this; defensive check for direct callers/tests. */
			return ALIRO_INVALID_ARGUMENT;
		}
		newPublicKey = existing.mPublicKey;
	}

	JournalRecord journal{};
	journal.mOp = JournalOp::CreateOrUpdate;
	journal.mHandle = handle;
	journal.mStagedKeyId = stagedKeyId;
	journal.mOldKeyId = (stagedKeyId != 0) ? oldKeyId : 0;

	auto error = Persistence::SaveJournal(journal);
	if (error != ALIRO_NO_ERROR) {
		LOG_ERR("Failed to journal create/update");
		if (stagedKeyId != 0) {
			KeyBackend::DestroyKey(stagedKeyId);
		}
		return error;
	}

	PersistedCredential record{};
	record.mValid = true;
	record.mHandle = handle;
	record.mKeyId = (stagedKeyId != 0) ? stagedKeyId : existing.mKeyId;
	record.mPublicKey = newPublicKey;
	ApplyPayloadToRecord(input, record);

	error = Persistence::SaveSlot(slotIndex, record);
	if (error != ALIRO_NO_ERROR) {
		LOG_ERR("Failed to persist credential slot");
		if (stagedKeyId != 0) {
			KeyBackend::DestroyKey(stagedKeyId);
		}
		Persistence::EraseJournal();
		return error;
	}

	sSlots[slotIndex] = record;

	if (journal.mOldKeyId != 0 && journal.mOldKeyId != record.mKeyId) {
		KeyBackend::DestroyKey(journal.mOldKeyId);
	}

	Persistence::EraseJournal();

	outHandle = handle;
	return ALIRO_NO_ERROR;
}

/* Boot-time journal recovery: finish-forward if the metadata switch already landed, else roll back. */
void RecoverJournal()
{
	JournalRecord journal{};
	bool present{ false };

	if (Persistence::LoadJournal(journal, present) != ALIRO_NO_ERROR || !present ||
	    journal.mOp == JournalOp::None) {
		return;
	}

	size_t slotIndex{};
	if (!HandleToSlotIndex(journal.mHandle, slotIndex)) {
		LOG_ERR("Journal record has out-of-range handle %u; discarding", journal.mHandle);
		Persistence::EraseJournal();
		return;
	}

	if (journal.mOp == JournalOp::CreateOrUpdate) {
		const bool metadataSwitched = sSlots[slotIndex].mValid && sSlots[slotIndex].mKeyId == journal.mStagedKeyId &&
					      journal.mStagedKeyId != 0;

		if (metadataSwitched) {
			LOG_INF("Finishing interrupted transaction for handle %u (metadata already switched)",
				journal.mHandle);
			if (journal.mOldKeyId != 0 && journal.mOldKeyId != sSlots[slotIndex].mKeyId) {
				KeyBackend::DestroyKey(journal.mOldKeyId);
			}
		} else {
			LOG_WRN("Rolling back interrupted transaction for handle %u", journal.mHandle);
			if (journal.mStagedKeyId != 0) {
				KeyBackend::DestroyKey(journal.mStagedKeyId);
			}
		}
	} else if (journal.mOp == JournalOp::Delete) {
		if (sSlots[slotIndex].mValid) {
			LOG_INF("Finishing interrupted delete for handle %u", journal.mHandle);
			Persistence::EraseSlot(slotIndex);
			sSlots[slotIndex] = PersistedCredential{};
		}
		KeyBackend::DestroyKey(journal.mOldKeyId);
		PurgePreferredEntriesForHandle(journal.mHandle);
	}

	Persistence::EraseJournal();
}

} // namespace

AliroError Init()
{
	Lock lock;

	auto error = Persistence::Init();
	VerifyOrReturnStatus(error == ALIRO_NO_ERROR, error, LOG_ERR("Persistence init failed"));

	error = KeyBackend::Init();
	VerifyOrReturnStatus(error == ALIRO_NO_ERROR, error, LOG_ERR("Key backend init failed"));

	for (size_t i = 0; i < kMaxCredentials; ++i) {
		bool present{ false };
		error = Persistence::LoadSlot(i, sSlots[i], present);
		if (error != ALIRO_NO_ERROR) {
			LOG_ERR("Failed to load credential slot %zu", i);
			sSlots[i] = PersistedCredential{};
		} else if (!present) {
			sSlots[i] = PersistedCredential{};
		}
	}

	RecoverJournal();

	error = Persistence::LoadPreferredTable(sPreferred);
	if (error != ALIRO_NO_ERROR) {
		LOG_ERR("Failed to load preferred-credential table");
		sPreferred = PreferredTable{};
	}

	return ALIRO_NO_ERROR;
}

AliroError Validate(const Provisioning::Payload &input)
{
	Lock lock;
	return ValidatePayloadShape(input);
}

AliroError Create(const Provisioning::Payload &input, CredentialHandle &outHandle)
{
	outHandle = kInvalidCredentialHandle;
	Lock lock;

	if (!input.mHasNewKeyInput) {
		LOG_WRN("Create() requires a new key input");
		return ALIRO_INVALID_ARGUMENT;
	}

	auto error = ValidatePayloadShape(input);
	VerifyOrReturnStatus(error == ALIRO_NO_ERROR, error);

	size_t freeSlot{ kMaxCredentials };
	for (size_t i = 0; i < kMaxCredentials; ++i) {
		if (!sSlots[i].mValid) {
			freeSlot = i;
			break;
		}
	}

	if (freeSlot == kMaxCredentials) {
		LOG_WRN("No free credential slot (capacity %zu)", kMaxCredentials);
		return ALIRO_NO_MEMORY;
	}

	return CommitTransaction(freeSlot, input, /*isUpdate=*/false, outHandle);
}

AliroError Update(CredentialHandle handle, const Provisioning::Payload &input)
{
	Lock lock;

	size_t slotIndex{};
	if (!HandleToSlotIndex(handle, slotIndex) || !sSlots[slotIndex].mValid) {
		return ALIRO_INVALID_ARGUMENT;
	}

	auto error = ValidatePayloadShape(input);
	VerifyOrReturnStatus(error == ALIRO_NO_ERROR, error);

	CredentialHandle unusedOutHandle{};
	return CommitTransaction(slotIndex, input, /*isUpdate=*/true, unusedOutHandle);
}

AliroError Delete(CredentialHandle handle)
{
	Lock lock;

	size_t slotIndex{};
	if (!HandleToSlotIndex(handle, slotIndex) || !sSlots[slotIndex].mValid) {
		return ALIRO_INVALID_ARGUMENT;
	}

	return DeleteSlotInternal(slotIndex);
}

AliroError Reset()
{
	Lock lock;

	AliroError firstError{ ALIRO_NO_ERROR };

	for (size_t i = 0; i < kMaxCredentials; ++i) {
		if (sSlots[i].mValid) {
			const auto error = DeleteSlotInternal(i);
			if (error != ALIRO_NO_ERROR && firstError == ALIRO_NO_ERROR) {
				firstError = error;
			}
		}
	}

	sPreferred = PreferredTable{};
	const auto error = Persistence::SavePreferredTable(sPreferred);
	if (error != ALIRO_NO_ERROR && firstError == ALIRO_NO_ERROR) {
		firstError = error;
	}

	return firstError;
}

AliroError GetMetadata(CredentialHandle handle, CredentialMetadata &outMetadata)
{
	outMetadata = CredentialMetadata{};
	Lock lock;

	size_t slotIndex{};
	if (!HandleToSlotIndex(handle, slotIndex) || !sSlots[slotIndex].mValid) {
		return ALIRO_INVALID_ARGUMENT;
	}

	const auto &record = sSlots[slotIndex];
	outMetadata.mHandle = handle;
	outMetadata.mReaderGroupBindingCount = record.mBindingCount;
	outMetadata.mAuthenticationPolicy = record.mPolicy;
	outMetadata.mHasReaderTrust = record.mBindingCount > 0;
	outMetadata.mHasMailbox = record.mMailbox.mConfigured;
	outMetadata.mHasCredentialSignedTimestamp = record.mHasCredentialSignedTimestamp;
	outMetadata.mHasRevocationSignedTimestamp = record.mHasRevocationSignedTimestamp;

	return ALIRO_NO_ERROR;
}

AliroError GetFullRecord(CredentialHandle handle, PersistedCredential &out)
{
	out = PersistedCredential{};
	Lock lock;

	size_t slotIndex{};
	if (!HandleToSlotIndex(handle, slotIndex) || !sSlots[slotIndex].mValid) {
		return ALIRO_INVALID_ARGUMENT;
	}

	out = sSlots[slotIndex];
	return ALIRO_NO_ERROR;
}

AliroError GetGroupBindingCount(CredentialHandle handle, size_t &outCount)
{
	outCount = 0;
	Lock lock;

	size_t slotIndex{};
	if (!HandleToSlotIndex(handle, slotIndex) || !sSlots[slotIndex].mValid) {
		return ALIRO_INVALID_ARGUMENT;
	}

	outCount = sSlots[slotIndex].mBindingCount;
	return ALIRO_NO_ERROR;
}

AliroError GetGroupBinding(CredentialHandle handle, size_t index, ReaderGroupIdentifier &outIdentifier)
{
	outIdentifier = ReaderGroupIdentifier{};
	Lock lock;

	size_t slotIndex{};
	if (!HandleToSlotIndex(handle, slotIndex) || !sSlots[slotIndex].mValid) {
		return ALIRO_INVALID_ARGUMENT;
	}

	const auto &record = sSlots[slotIndex];
	if (index >= record.mBindingCount) {
		return ALIRO_INVALID_ARGUMENT;
	}

	outIdentifier = record.mBindings[index].mReaderGroupIdentifier;
	return ALIRO_NO_ERROR;
}

AliroError ResolveByReaderGroupIdentifier(const ReaderGroupIdentifier &readerGroupIdentifier,
					  CredentialHandle *outHandles, size_t &inOutCount)
{
	Lock lock;

	const size_t capacity = inOutCount;
	size_t written = 0;
	CredentialHandle preferredHandle{ kInvalidCredentialHandle };

	for (const auto &entry : sPreferred.mEntries) {
		if (entry.mValid && ReaderGroupIdentifierEquals(entry.mReaderGroupIdentifier, readerGroupIdentifier)) {
			preferredHandle = entry.mPreferredHandle;
			break;
		}
	}

	/* Preferred match first, if it actually matches. */
	if (preferredHandle != kInvalidCredentialHandle) {
		size_t preferredSlot{};
		if (HandleToSlotIndex(preferredHandle, preferredSlot) && sSlots[preferredSlot].mValid) {
			const auto &record = sSlots[preferredSlot];
			for (uint32_t b = 0; b < record.mBindingCount; ++b) {
				if (ReaderGroupIdentifierEquals(record.mBindings[b].mReaderGroupIdentifier,
								readerGroupIdentifier) &&
				    written < capacity) {
					outHandles[written++] = preferredHandle;
					break;
				}
			}
		}
	}

	for (size_t i = 0; i < kMaxCredentials && written < capacity; ++i) {
		if (!sSlots[i].mValid) {
			continue;
		}

		const CredentialHandle handle = SlotIndexToHandle(i);
		if (handle == preferredHandle) {
			continue;
		}

		for (uint32_t b = 0; b < sSlots[i].mBindingCount; ++b) {
			if (ReaderGroupIdentifierEquals(sSlots[i].mBindings[b].mReaderGroupIdentifier,
							readerGroupIdentifier)) {
				outHandles[written++] = handle;
				break;
			}
		}
	}

	inOutCount = written;
	return ALIRO_NO_ERROR;
}

namespace {

/*
 * Shared lookup for GetReaderPublicKey()/GetReaderIssuerPublicKey(): always
 * scans every slot and every binding, regardless of where/whether a match
 * is found, and never returns early on a mismatch, so an invalid handle, an
 * absent binding, and a wrong-trust-type binding are computationally
 * indistinguishable (ALIRO-UD-SYRS-P1-024/031).
 */
AliroError LookupBindingKey(CredentialHandle handle, const ReaderGroupIdentifier &readerGroupIdentifier,
			    TrustType wantedType, CryptoTypes::PublicKey &outPublicKey)
{
	outPublicKey = CryptoTypes::PublicKey{};

	size_t targetSlot{ kMaxCredentials };
	HandleToSlotIndex(handle, targetSlot);

	bool found = false;
	CryptoTypes::PublicKey foundKey{};

	for (size_t i = 0; i < kMaxCredentials; ++i) {
		const bool slotMatches = (i == targetSlot) && sSlots[i].mValid;

		for (size_t b = 0; b < kMaxBindingsPerCredential; ++b) {
			const auto &binding = sSlots[i].mBindings[b];
			const bool bindingMatches = slotMatches && (b < sSlots[i].mBindingCount) &&
						    binding.mTrustType == wantedType &&
						    ReaderGroupIdentifierEquals(binding.mReaderGroupIdentifier,
										readerGroupIdentifier);

			if (bindingMatches && !found) {
				found = true;
				foundKey = binding.mKey;
			}
		}
	}

	if (!found) {
		return ALIRO_PUBLIC_KEY_NOT_FOUND;
	}

	outPublicKey = foundKey;
	return ALIRO_NO_ERROR;
}

} // namespace

AliroError GetReaderPublicKey(CredentialHandle handle, const ReaderGroupIdentifier &readerGroupIdentifier,
			      CryptoTypes::PublicKey &outPublicKey)
{
	Lock lock;
	return LookupBindingKey(handle, readerGroupIdentifier, TrustType::Direct, outPublicKey);
}

AliroError GetReaderIssuerPublicKey(CredentialHandle handle, const ReaderGroupIdentifier &readerGroupIdentifier,
				    CryptoTypes::PublicKey &outPublicKey)
{
	Lock lock;
	return LookupBindingKey(handle, readerGroupIdentifier, TrustType::IssuerCa, outPublicKey);
}

AliroError SetPreferredCredential(const ReaderGroupIdentifier &readerGroupIdentifier, CredentialHandle handle)
{
	Lock lock;

	size_t slotIndex{};
	if (!HandleToSlotIndex(handle, slotIndex) || !sSlots[slotIndex].mValid) {
		return ALIRO_INVALID_ARGUMENT;
	}

	bool hasBinding = false;
	for (uint32_t b = 0; b < sSlots[slotIndex].mBindingCount; ++b) {
		if (ReaderGroupIdentifierEquals(sSlots[slotIndex].mBindings[b].mReaderGroupIdentifier,
						readerGroupIdentifier)) {
			hasBinding = true;
			break;
		}
	}

	if (!hasBinding) {
		LOG_WRN("Credential %u has no binding for the given reader_group_identifier", handle);
		return ALIRO_INVALID_ARGUMENT;
	}

	PreferredBinding *freeEntry = nullptr;
	for (auto &entry : sPreferred.mEntries) {
		if (entry.mValid && ReaderGroupIdentifierEquals(entry.mReaderGroupIdentifier, readerGroupIdentifier)) {
			entry.mPreferredHandle = handle;
			return Persistence::SavePreferredTable(sPreferred);
		}

		if (!entry.mValid && freeEntry == nullptr) {
			freeEntry = &entry;
		}
	}

	if (freeEntry == nullptr) {
		LOG_WRN("Preferred-credential table full (capacity %zu)", kMaxPreferredBindings);
		return ALIRO_NO_MEMORY;
	}

	freeEntry->mValid = true;
	freeEntry->mReaderGroupIdentifier = readerGroupIdentifier;
	freeEntry->mPreferredHandle = handle;

	return Persistence::SavePreferredTable(sPreferred);
}

AliroError GetPreferredCredential(const ReaderGroupIdentifier &readerGroupIdentifier, CredentialHandle &outHandle)
{
	outHandle = kInvalidCredentialHandle;
	Lock lock;

	for (const auto &entry : sPreferred.mEntries) {
		if (entry.mValid && ReaderGroupIdentifierEquals(entry.mReaderGroupIdentifier, readerGroupIdentifier)) {
			outHandle = entry.mPreferredHandle;
			return ALIRO_NO_ERROR;
		}
	}

	return ALIRO_ERROR_UNKNOWN;
}

} // namespace AliroUd::Credential::Store
