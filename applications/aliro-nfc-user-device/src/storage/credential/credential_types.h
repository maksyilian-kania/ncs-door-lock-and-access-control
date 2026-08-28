/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#pragma once

#include <aliro/types.h>
#include <aliro/user_device/types.h>

#include <array>
#include <cstdint>

/**
 * @brief Credential/trust persistence data model (APP_PLAN.md AWP3).
 *
 * Every type here is plain-old-data so it can be persisted verbatim
 * (`credential_persistence.h`) and so tests can construct/compare instances
 * directly. None of these types are part of any `ncs-aliro` public contract:
 * they are this application's own storage representation, produced from and
 * consumed only through the CLI-facing wire format in `provisioning.h` and
 * the `Aliro::Interface::UserDevice::Credential`/`::Trust` adapter in
 * `credential.cpp`.
 */
namespace AliroUd::Credential {

/** @brief Maximum number of provisioned Access Credentials. */
constexpr size_t kMaxCredentials{ CONFIG_ALIRO_UD_MAX_CREDENTIALS };

/** @brief Maximum number of reader_group_identifier bindings per credential. */
constexpr size_t kMaxBindingsPerCredential{ CONFIG_ALIRO_UD_MAX_BINDINGS_PER_CREDENTIAL };

/** @brief Maximum provisionable mailbox size, in bytes. */
constexpr size_t kMailboxMaxSizeBytes{ CONFIG_ALIRO_UD_MAILBOX_MAX_SIZE };

/** @brief Maximum size of each optional Access/Revocation Document, in bytes. */
constexpr size_t kDocumentMaxSizeBytes{ CONFIG_ALIRO_UD_DOCUMENT_MAX_SIZE };

/** @brief Maximum number of distinct reader_group_identifier values with a recorded preference. */
constexpr size_t kMaxPreferredBindings{ CONFIG_ALIRO_UD_MAX_PREFERRED_BINDINGS };

/**
 * @brief Which trust material a `reader_group_identifier` binding carries
 * (APP_PLAN.md AWP3, WP5.5 decision D9): a directly-bound Reader public key,
 * or a Reader System Issuer CA public key used to validate certificates
 * presented by any Reader in that group.
 */
enum class TrustType : uint8_t {
	/** No binding is provisioned in this slot. */
	None = 0,
	/** `mKey` is the directly-trusted Reader public key. */
	Direct = 1,
	/** `mKey` is the trusted Reader System Issuer CA public key. */
	IssuerCa = 2,
};

/** @brief One `reader_group_identifier` trust binding. */
struct Binding {
	::Aliro::UserDevice::ReaderGroupIdentifier mReaderGroupIdentifier{};
	TrustType mTrustType{ TrustType::None };
	::Aliro::CryptoTypes::PublicKey mKey{};

	bool IsEmpty() const { return mTrustType == TrustType::None; }
};

/** @brief Staged/committed mailbox configuration (byte storage implemented in AWP6). */
struct MailboxConfig {
	bool mConfigured{ false };
	uint32_t mSizeBytes{ 0 };
	bool mReadable{ false };
	bool mWritable{ false };
	bool mSettableInAuth1{ false };
};

/** @brief One optional, opaque, application-stored document (Access Document or Revocation Document). */
struct OptionalDocument {
	bool mPresent{ false };
	uint32_t mLength{ 0 };
	std::array<uint8_t, kDocumentMaxSizeBytes> mData{};
};

/**
 * @brief Committed, persisted credential record.
 *
 * Never contains the Access Credential private key: only the opaque PSA key
 * identifier returned when the key was imported. `mPublicKey` is the
 * corresponding (non-secret) derived public key.
 */
struct PersistedCredential {
	bool mValid{ false };
	::Aliro::UserDevice::CredentialHandle mHandle{ ::Aliro::UserDevice::kInvalidCredentialHandle };
	::Aliro::CryptoTypes::KeyId mKeyId{ 0 };
	::Aliro::CryptoTypes::PublicKey mPublicKey{};
	::Aliro::UserDevice::AuthenticationPolicy mPolicy{ ::Aliro::UserDevice::AuthenticationPolicy::UserDeviceSetting };
	uint32_t mBindingCount{ 0 };
	std::array<Binding, kMaxBindingsPerCredential> mBindings{};
	MailboxConfig mMailbox{};
	bool mHasCredentialSignedTimestamp{ false };
	::Aliro::Timestamp mCredentialSignedTimestamp{};
	bool mHasRevocationSignedTimestamp{ false };
	::Aliro::Timestamp mRevocationSignedTimestamp{};
	OptionalDocument mAccessDocument{};
	OptionalDocument mRevocationDocument{};
};

/**
 * @brief One recorded "preferred credential" entry for a shared
 * `reader_group_identifier` bound by more than one credential
 * (ALIRO-UD-SYRS-P1-010).
 */
struct PreferredBinding {
	bool mValid{ false };
	::Aliro::UserDevice::ReaderGroupIdentifier mReaderGroupIdentifier{};
	::Aliro::UserDevice::CredentialHandle mPreferredHandle{ ::Aliro::UserDevice::kInvalidCredentialHandle };
};

/** @brief The whole preferred-credential table, persisted as a single record. */
struct PreferredTable {
	std::array<PreferredBinding, kMaxPreferredBindings> mEntries{};
};

/**
 * @brief Persistent transaction journal operation kind
 * (APP_PLAN.md AWP3: "an application-specific persistent transaction
 * journal spanning NVS and PSA key storage").
 *
 * Factory reset has no dedicated op: it is implemented as a sequence of
 * individually journaled, individually crash-safe per-credential deletions
 * followed by one atomic preferred-table clear, so an interruption anywhere
 * during a reset leaves the store in a valid (partially reset) state with
 * no orphaned keys, recoverable by simply calling reset again.
 */
enum class JournalOp : uint8_t {
	/** No transaction is in flight. */
	None = 0,
	/** Create (no prior record) or update (prior record exists) a credential. */
	CreateOrUpdate = 1,
	/** Delete a credential. */
	Delete = 2,
};

/**
 * @brief One journal record describing the single in-flight mutating
 * transaction, if any.
 *
 * At most one mutating transaction is ever in flight (the lifecycle
 * coordinator, `src/lifecycle`, serializes every mutating call), so a single
 * fixed-size record is sufficient; no dynamic log is needed.
 */
struct JournalRecord {
	JournalOp mOp{ JournalOp::None };
	::Aliro::UserDevice::CredentialHandle mHandle{ ::Aliro::UserDevice::kInvalidCredentialHandle };
	/** @brief Newly-imported key for this transaction; 0 if none was imported. */
	::Aliro::CryptoTypes::KeyId mStagedKeyId{ 0 };
	/** @brief Previously-committed key being replaced/removed; 0 if none. */
	::Aliro::CryptoTypes::KeyId mOldKeyId{ 0 };
};

/**
 * @brief In-memory CLI staging candidate (APP_PLAN.md AWP3 "CLI-driven
 * staging transaction").
 *
 * May transiently hold the raw Access Credential private-key scalar between
 * `set-key` and `commit`/`abort`; never persisted in this form.
 */
struct StagingCandidate {
	bool mActive{ false };
	bool mIsUpdate{ false };
	::Aliro::UserDevice::CredentialHandle mBaseHandle{ ::Aliro::UserDevice::kInvalidCredentialHandle };
	bool mHasNewKeyInput{ false };
	std::array<uint8_t, ::Aliro::CryptoTypes::kEccP256KeyPrivateKeyLength> mNewKeyScalar{};
	bool mPolicySet{ false };
	::Aliro::UserDevice::AuthenticationPolicy mPolicy{ ::Aliro::UserDevice::AuthenticationPolicy::UserDeviceSetting };
	uint32_t mBindingCount{ 0 };
	std::array<Binding, kMaxBindingsPerCredential> mBindings{};
	MailboxConfig mMailbox{};
	bool mHasCredentialSignedTimestamp{ false };
	::Aliro::Timestamp mCredentialSignedTimestamp{};
	bool mHasRevocationSignedTimestamp{ false };
	::Aliro::Timestamp mRevocationSignedTimestamp{};
	OptionalDocument mAccessDocument{};
	OptionalDocument mRevocationDocument{};

	/** @brief Erases the scalar (and everything else); called after commit/abort. */
	void Clear() { *this = StagingCandidate{}; }
};

} // namespace AliroUd::Credential
