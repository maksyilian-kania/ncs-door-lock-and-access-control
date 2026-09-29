/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include <app_version.h>
#include <zephyr/kernel.h>
#include <zephyr/shell/shell.h>
#include <zephyr/sys/iterable_sections.h>
#include <zephyr/sys/util.h>

#include "lifecycle/lifecycle.h"
#include "platform/authorization/authorization_indicator.h"
#include "platform/authorization/authorization_window.h"
#include "platform/nfc/command_timing.h"
#include "platform/nfc/nfc_worker.h"
#include "platform/os/app_status.h"
#include "storage/credential/credential_store.h"
#include "storage/credential/credential_types.h"
#include "storage/credential/provisioning.h"
#include "storage/mailbox/mailbox_store.h"

#include <aliro/user_device/interface.h>
#include <aliro/user_device/user_device.h>

#include <cstdlib>
#include <cstring>
#include <limits>

/*
 * Development CLI over the DK virtual UART (APP_PLAN.md AWP2/AWP3): a
 * Zephyr shell "aliro-ud" root command with an "info" command and an
 * "aliro-ud credential" group implementing the CLI-driven credential
 * staging transaction plus deterministic create/update/inspect/delete/
 * factory-reset/binding-enumeration/preferred-credential commands.
 *
 * Every handler prints exactly one deterministic, machine-readable
 * "OK ..."/"ERR ..." line and never a secret value (the raw private-key
 * scalar staged by `set-key` is held only in the in-memory
 * `AliroUd::Credential::StagingCandidate` below and is never echoed back).
 *
 * Every mutating command (`commit`, `delete`, `reset`) reaches credential
 * management only through `Aliro::UserDeviceStack`'s facade (WP5.5,
 * decision D7), wrapped in `AliroUd::Lifecycle::RunMutation()` so it runs
 * with NFC activation paused and any active session torn down first
 * (APP_PLAN.md AWP3 lifecycle coordinator). `begin-update`'s non-secret
 * field clone is the one place this file reaches
 * `AliroUd::Credential::Store` directly: the facade's read surface is
 * deliberately metadata-only and has no accessor for mailbox/document
 * staging fields.
 */
namespace AliroUd::Cli {
namespace {

using AliroUd::Credential::Binding;
using AliroUd::Credential::kDocumentMaxSizeBytes;
using AliroUd::Credential::kMailboxMaxSizeBytes;
using AliroUd::Credential::kMaxBindingsPerCredential;
using AliroUd::Credential::kMaxCredentials;
using AliroUd::Credential::StagingCandidate;
using AliroUd::Credential::TrustType;

/* One in-memory staging transaction; see credential_types.h. Only one CLI shell thread drives this. */
StagingCandidate sCandidate{};

void PrintError(const struct shell *sh, const char *command, AliroError error)
{
	shell_print(sh, "ERR %d command=%s", error.ToInt(), command);
}

int HexNibble(char c)
{
	if (c >= '0' && c <= '9') {
		return c - '0';
	}
	if (c >= 'a' && c <= 'f') {
		return c - 'a' + 10;
	}
	if (c >= 'A' && c <= 'F') {
		return c - 'A' + 10;
	}
	return -1;
}

/* Exactly `outLen` bytes of hex digits; `out` is written only if the whole input is valid. */
bool ParseHexBytes(const char *hex, uint8_t *out, size_t outLen)
{
	if (strlen(hex) != outLen * 2) {
		return false;
	}

	for (size_t i = 0; i < outLen * 2; ++i) {
		if (HexNibble(hex[i]) < 0) {
			return false;
		}
	}

	for (size_t i = 0; i < outLen; ++i) {
		out[i] = static_cast<uint8_t>((HexNibble(hex[2 * i]) << 4) | HexNibble(hex[2 * i + 1]));
	}

	return true;
}

template <size_t N> bool ParseHexArray(const char *hex, std::array<uint8_t, N> &out)
{
	return ParseHexBytes(hex, out.data(), N);
}

/* Decimal digits only: no sign, whitespace, or prefix. An out-of-range value saturates to ULONG_MAX. */
bool ParseUint(const char *str, unsigned long &out)
{
	if (*str == '\0') {
		return false;
	}

	for (const char *c = str; *c != '\0'; ++c) {
		if (*c < '0' || *c > '9') {
			return false;
		}
	}

	out = strtoul(str, nullptr, 10);
	return true;
}

bool ParseHandle(const char *str, ::Aliro::UserDevice::CredentialHandle &out)
{
	unsigned long value{};
	if (!ParseUint(str, value) || value > std::numeric_limits<::Aliro::UserDevice::CredentialHandle>::max()) {
		return false;
	}

	out = static_cast<::Aliro::UserDevice::CredentialHandle>(value);
	return true;
}

bool ParseDocumentType(const char *str, ::Aliro::AccessDocumentTypes::DocumentType &out)
{
	if (strcmp(str, "access") == 0) {
		out = ::Aliro::AccessDocumentTypes::DocumentType::Access;
		return true;
	}
	if (strcmp(str, "revocation") == 0) {
		out = ::Aliro::AccessDocumentTypes::DocumentType::Revocation;
		return true;
	}
	return false;
}

int CmdInfo(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	shell_print(sh,
		    "OK version=%s init=%s session_active=%u activation_attempts=%zu "
		    "rejected_apdus=%zu",
		    APP_VERSION_EXTENDED_STRING, AliroUd::AppStatus::ToString(AliroUd::AppStatus::GetInitState()),
		    AliroUd::Nfc::IsSessionActive() ? 1U : 0U, AliroUd::Nfc::GetActivationAttemptCount(),
		    AliroUd::Nfc::GetRejectedApduCount());
	return 0;
}

/*
 * "aliro-ud auth" (APP_PLAN.md AWP4): read-only status plus a test trigger
 * that opens the button authorization window without physical DK hardware,
 * for host tests and development ("Provide an application test trigger if
 * the current stack cannot request authorization end to end").
 */
int CmdAuthStatus(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	const int64_t now = k_uptime_get();
	const auto state = AliroUd::Authorization::GlobalWindow().GetState(now);
	const bool authorized = state == ::Aliro::UserDevice::AuthorizationState::Authorized;

	shell_print(sh, "OK state=%s remaining_ms=%lld", authorized ? "authorized" : "required",
		    static_cast<long long>(AliroUd::Authorization::GlobalWindow().GetRemainingMs(now)));
	return 0;
}

int CmdAuthPress(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	AliroUd::Authorization::GlobalWindow().Open(
		k_uptime_get(), static_cast<uint32_t>(CONFIG_ALIRO_UD_AUTHORIZATION_WINDOW_SECONDS) * 1000U);
	AliroUd::Authorization::Indicator::SetActive(false);

	shell_print(sh, "OK");
	return 0;
}

int CmdAuthClear(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	AliroUd::Authorization::GlobalWindow().Close();

	shell_print(sh, "OK");
	return 0;
}

/*
 * Test trigger for the other half of the Authorization contract
 * (APP_PLAN.md AWP4's "Provide an application test trigger if the current
 * stack cannot request authorization end to end"): calls the same
 * `Aliro::Interface::UserDevice::Authorization::NotifyAuthenticationRequired()`
 * the real stack calls when an AUTH0 policy 1-3 credential is used with no
 * valid button window, without needing a physical NFC reader tapping the
 * DK. Drives the same visible LED indication path.
 */
int CmdAuthNotifyRequired(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	::Aliro::Interface::UserDevice::Authorization::NotifyAuthenticationRequired(
		::Aliro::UserDevice::kInvalidCredentialHandle);

	shell_print(sh, "OK");
	return 0;
}

int CmdCredentialBeginCreate(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	if (sCandidate.mActive) {
		shell_print(sh, "ERR TRANSACTION_ACTIVE command=credential begin-create");
		return 0;
	}

	sCandidate = StagingCandidate{};
	sCandidate.mActive = true;
	sCandidate.mIsUpdate = false;
	shell_print(sh, "OK");
	return 0;
}

int CmdCredentialBeginUpdate(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);

	if (sCandidate.mActive) {
		shell_print(sh, "ERR TRANSACTION_ACTIVE command=credential begin-update");
		return 0;
	}

	::Aliro::UserDevice::CredentialHandle handle{};
	if (!ParseHandle(argv[1], handle)) {
		shell_print(sh, "ERR INVALID_ARGUMENT command=credential begin-update");
		return 0;
	}

	AliroUd::Credential::PersistedCredential record{};
	const auto error = AliroUd::Credential::Store::GetFullRecord(handle, record);
	if (error != ALIRO_NO_ERROR) {
		PrintError(sh, "credential begin-update", error);
		return 0;
	}

	sCandidate = StagingCandidate{};
	sCandidate.mActive = true;
	sCandidate.mIsUpdate = true;
	sCandidate.mBaseHandle = handle;
	sCandidate.mPolicySet = true;
	sCandidate.mPolicy = record.mPolicy;
	sCandidate.mBindingCount = record.mBindingCount;
	sCandidate.mBindings = record.mBindings;
	sCandidate.mMailbox = record.mMailbox;
	sCandidate.mHasCredentialSignedTimestamp = record.mHasCredentialSignedTimestamp;
	sCandidate.mCredentialSignedTimestamp = record.mCredentialSignedTimestamp;
	sCandidate.mHasRevocationSignedTimestamp = record.mHasRevocationSignedTimestamp;
	sCandidate.mRevocationSignedTimestamp = record.mRevocationSignedTimestamp;
	sCandidate.mAccessDocument = record.mAccessDocument;
	sCandidate.mRevocationDocument = record.mRevocationDocument;
	/* mHasNewKeyInput stays false: the existing opaque key is retained unless set-key is called. */

	shell_print(sh, "OK");
	return 0;
}

int CmdCredentialSetKey(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);

	if (!sCandidate.mActive) {
		shell_print(sh, "ERR NO_TRANSACTION command=credential set-key");
		return 0;
	}

	if (!ParseHexArray(argv[1], sCandidate.mNewKeyScalar)) {
		shell_print(sh, "ERR INVALID_ARGUMENT command=credential set-key");
		return 0;
	}

	sCandidate.mHasNewKeyInput = true;
	shell_print(sh, "OK");
	return 0;
}

int CmdCredentialSetBinding(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);

	if (!sCandidate.mActive) {
		shell_print(sh, "ERR NO_TRANSACTION command=credential set-binding");
		return 0;
	}

	unsigned long index{};
	if (!ParseUint(argv[1], index) || index >= kMaxBindingsPerCredential) {
		shell_print(sh, "ERR INVALID_ARGUMENT command=credential set-binding");
		return 0;
	}

	Binding binding{};
	if (!ParseHexArray(argv[2], binding.mReaderGroupIdentifier)) {
		shell_print(sh, "ERR INVALID_ARGUMENT command=credential set-binding");
		return 0;
	}

	if (strcmp(argv[3], "direct") == 0) {
		binding.mTrustType = TrustType::Direct;
	} else if (strcmp(argv[3], "issuer") == 0) {
		binding.mTrustType = TrustType::IssuerCa;
	} else {
		shell_print(sh, "ERR INVALID_ARGUMENT command=credential set-binding");
		return 0;
	}

	if (!ParseHexArray(argv[4], binding.mKey)) {
		shell_print(sh, "ERR INVALID_ARGUMENT command=credential set-binding");
		return 0;
	}

	sCandidate.mBindings[index] = binding;
	sCandidate.mBindingCount = MAX(sCandidate.mBindingCount, static_cast<uint32_t>(index) + 1);
	shell_print(sh, "OK");
	return 0;
}

int CmdCredentialSetPolicy(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);

	if (!sCandidate.mActive) {
		shell_print(sh, "ERR NO_TRANSACTION command=credential set-policy");
		return 0;
	}

	unsigned long value{};
	if (!ParseUint(argv[1], value) || value < 1 || value > 3) {
		shell_print(sh, "ERR INVALID_ARGUMENT command=credential set-policy");
		return 0;
	}

	sCandidate.mPolicySet = true;
	sCandidate.mPolicy = static_cast<::Aliro::UserDevice::AuthenticationPolicy>(value);
	shell_print(sh, "OK");
	return 0;
}

int CmdCredentialSetMailbox(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);

	if (!sCandidate.mActive) {
		shell_print(sh, "ERR NO_TRANSACTION command=credential set-mailbox");
		return 0;
	}

	unsigned long size{};
	unsigned long rights{};
	/*
	 * WP7 stack impact (see docs/wp7_stack_impact.md): `rights` is now a
	 * 2-bit mask (readable=0x1, writable=0x2). Bit 2 ("settable in
	 * AUTH1") was removed upstream - the AUTH1 mailbox_data_subset is
	 * read-only from the Reader's perspective - so `rights` > 3 is now
	 * rejected instead of silently accepted. Use
	 * "credential set-mailbox-data-subset" to provision the subset
	 * descriptor itself.
	 */
	if (!ParseUint(argv[1], size) || size > kMailboxMaxSizeBytes || !ParseUint(argv[2], rights) || rights > 3) {
		shell_print(sh, "ERR INVALID_ARGUMENT command=credential set-mailbox");
		return 0;
	}

	sCandidate.mMailbox.mConfigured = true;
	sCandidate.mMailbox.mSizeBytes = static_cast<uint32_t>(size);
	sCandidate.mMailbox.mReadable = (rights & 0x1) != 0;
	sCandidate.mMailbox.mWritable = (rights & 0x2) != 0;
	shell_print(sh, "OK");
	return 0;
}

/*
 * WP7 stack impact (see docs/wp7_stack_impact.md): stages one AUTH1
 * mailbox_data_subset (offset, length) pair. Mirrors
 * CmdCredentialSetBinding()'s indexed-slot pattern. `index` must be less
 * than CONFIG_ALIRO_UD_MAILBOX_MAX_DATA_SUBSET_PAIRS; bounds against the
 * mailbox's own provisioned size are re-checked at `commit` (validated
 * once the full candidate, including mSizeBytes, is known).
 */
int CmdCredentialSetMailboxDataSubset(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);

	if (!sCandidate.mActive) {
		shell_print(sh, "ERR NO_TRANSACTION command=credential set-mailbox-data-subset");
		return 0;
	}

	unsigned long index{};
	unsigned long offset{};
	unsigned long length{};
	if (!ParseUint(argv[1], index) || index >= AliroUd::Credential::kMaxMailboxDataSubsetPairs ||
	    !ParseUint(argv[2], offset) || offset > UINT16_MAX || !ParseUint(argv[3], length) || length > UINT16_MAX) {
		shell_print(sh, "ERR INVALID_ARGUMENT command=credential set-mailbox-data-subset");
		return 0;
	}

	sCandidate.mMailbox.mDataSubsetConfigured = true;
	sCandidate.mMailbox.mDataSubsetPairs[index].mOffset = static_cast<uint16_t>(offset);
	sCandidate.mMailbox.mDataSubsetPairs[index].mLength = static_cast<uint16_t>(length);
	sCandidate.mMailbox.mDataSubsetPairCount =
		MAX(sCandidate.mMailbox.mDataSubsetPairCount, static_cast<uint32_t>(index) + 1);
	shell_print(sh, "OK");
	return 0;
}

int CmdCredentialSetCredentialTimestamp(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);

	if (!sCandidate.mActive) {
		shell_print(sh, "ERR NO_TRANSACTION command=credential set-credential-timestamp");
		return 0;
	}

	if (!ParseHexArray(argv[1], sCandidate.mCredentialSignedTimestamp)) {
		shell_print(sh, "ERR INVALID_ARGUMENT command=credential set-credential-timestamp");
		return 0;
	}

	sCandidate.mHasCredentialSignedTimestamp = true;
	shell_print(sh, "OK");
	return 0;
}

int CmdCredentialSetRevocationTimestamp(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);

	if (!sCandidate.mActive) {
		shell_print(sh, "ERR NO_TRANSACTION command=credential set-revocation-timestamp");
		return 0;
	}

	if (!ParseHexArray(argv[1], sCandidate.mRevocationSignedTimestamp)) {
		shell_print(sh, "ERR INVALID_ARGUMENT command=credential set-revocation-timestamp");
		return 0;
	}

	sCandidate.mHasRevocationSignedTimestamp = true;
	shell_print(sh, "OK");
	return 0;
}

AliroUd::Credential::OptionalDocument &StagedDocument(::Aliro::AccessDocumentTypes::DocumentType type)
{
	return (type == ::Aliro::AccessDocumentTypes::DocumentType::Access) ? sCandidate.mAccessDocument
									     : sCandidate.mRevocationDocument;
}

bool &DocumentStaged(::Aliro::AccessDocumentTypes::DocumentType type)
{
	return (type == ::Aliro::AccessDocumentTypes::DocumentType::Access) ? sCandidate.mAccessDocumentStaged
									     : sCandidate.mRevocationDocumentStaged;
}

/*
 * The declared length guards against a truncated UART line. A document is
 * a non-empty CBOR structure (Aliro 1.0 Specification, section 7.2, page
 * 32), so length 0 is rejected; "clear-document" removes a document.
 */
int CmdCredentialSetDocument(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);

	if (!sCandidate.mActive) {
		shell_print(sh, "ERR NO_TRANSACTION command=credential set-document");
		return 0;
	}

	::Aliro::AccessDocumentTypes::DocumentType type{};
	unsigned long length{};
	if (!ParseDocumentType(argv[1], type) || !ParseUint(argv[2], length) || length == 0 ||
	    length > kDocumentMaxSizeBytes) {
		shell_print(sh, "ERR INVALID_ARGUMENT command=credential set-document");
		return 0;
	}

	if (DocumentStaged(type)) {
		shell_print(sh, "ERR DUPLICATE_FIELD command=credential set-document");
		return 0;
	}

	AliroUd::Credential::OptionalDocument document{};
	document.mPresent = true;
	document.mLength = static_cast<uint32_t>(length);
	if (!ParseHexBytes(argv[3], document.mData.data(), document.mLength)) {
		shell_print(sh, "ERR INVALID_ARGUMENT command=credential set-document");
		return 0;
	}

	StagedDocument(type) = document;
	DocumentStaged(type) = true;
	shell_print(sh, "OK");
	return 0;
}

int CmdCredentialClearDocument(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);

	if (!sCandidate.mActive) {
		shell_print(sh, "ERR NO_TRANSACTION command=credential clear-document");
		return 0;
	}

	::Aliro::AccessDocumentTypes::DocumentType type{};
	if (!ParseDocumentType(argv[1], type)) {
		shell_print(sh, "ERR INVALID_ARGUMENT command=credential clear-document");
		return 0;
	}

	if (DocumentStaged(type)) {
		shell_print(sh, "ERR DUPLICATE_FIELD command=credential clear-document");
		return 0;
	}

	StagedDocument(type) = AliroUd::Credential::OptionalDocument{};
	DocumentStaged(type) = true;
	shell_print(sh, "OK");
	return 0;
}

int CmdCredentialCommit(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	if (!sCandidate.mActive) {
		shell_print(sh, "ERR NO_TRANSACTION command=credential commit");
		return 0;
	}

	auto payload = AliroUd::Credential::Provisioning::FromStagingCandidate(sCandidate);
	const bool isUpdate = sCandidate.mIsUpdate;
	const auto baseHandle = sCandidate.mBaseHandle;
	::Aliro::UserDevice::CredentialHandle newHandle{ ::Aliro::UserDevice::kInvalidCredentialHandle };

	const AliroError error = AliroUd::Lifecycle::RunMutation([&]() -> AliroError {
		const auto constData = AliroUd::Credential::Provisioning::AsConstData(payload);
		if (isUpdate) {
			return Aliro::UserDeviceStack::Instance().UpdateCredential(baseHandle, constData);
		}
		return Aliro::UserDeviceStack::Instance().CreateCredential(constData, newHandle);
	});

	/* Defense in depth: the raw scalar never needs to outlive this call. */
	payload.mNewKeyScalar.fill(0);
	sCandidate.Clear();

	if (error != ALIRO_NO_ERROR) {
		PrintError(sh, "credential commit", error);
		return 0;
	}

	if (isUpdate) {
		shell_print(sh, "OK handle=%u", static_cast<unsigned>(baseHandle));
	} else {
		shell_print(sh, "OK handle=%u", static_cast<unsigned>(newHandle));
	}
	return 0;
}

int CmdCredentialAbort(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	if (!sCandidate.mActive) {
		shell_print(sh, "ERR NO_TRANSACTION command=credential abort");
		return 0;
	}

	sCandidate.Clear();
	shell_print(sh, "OK");
	return 0;
}

void PrintMetadataLine(const struct shell *sh, const ::Aliro::UserDevice::CredentialMetadata &meta)
{
	shell_print(sh,
		    "OK handle=%u bindings=%zu policy=%u has_trust=%u has_mailbox=%u "
		    "has_credential_timestamp=%u has_revocation_timestamp=%u",
		    static_cast<unsigned>(meta.mHandle), meta.mReaderGroupBindingCount,
		    static_cast<unsigned>(meta.mAuthenticationPolicy), meta.mHasReaderTrust ? 1U : 0U,
		    meta.mHasMailbox ? 1U : 0U, meta.mHasCredentialSignedTimestamp ? 1U : 0U,
		    meta.mHasRevocationSignedTimestamp ? 1U : 0U);
}

int CmdCredentialInspect(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);

	::Aliro::UserDevice::CredentialHandle handle{};
	if (!ParseHandle(argv[1], handle)) {
		shell_print(sh, "ERR INVALID_ARGUMENT command=credential inspect");
		return 0;
	}

	::Aliro::UserDevice::CredentialMetadata meta{};
	const auto error = Aliro::UserDeviceStack::Instance().GetCredentialMetadata(handle, meta);

	if (error != ALIRO_NO_ERROR) {
		PrintError(sh, "credential inspect", error);
		return 0;
	}

	PrintMetadataLine(sh, meta);
	return 0;
}

int CmdCredentialList(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	size_t count = 0;
	for (::Aliro::UserDevice::CredentialHandle handle = 1; handle <= kMaxCredentials; ++handle) {
		::Aliro::UserDevice::CredentialMetadata meta{};
		if (Aliro::UserDeviceStack::Instance().GetCredentialMetadata(handle, meta) == ALIRO_NO_ERROR) {
			PrintMetadataLine(sh, meta);
			++count;
		}
	}

	shell_print(sh, "OK count=%zu", count);
	return 0;
}

int CmdCredentialDelete(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);

	::Aliro::UserDevice::CredentialHandle handle{};
	if (!ParseHandle(argv[1], handle)) {
		shell_print(sh, "ERR INVALID_ARGUMENT command=credential delete");
		return 0;
	}

	/* The Credential adapter also erases the credential's mailbox (storage/credential/credential.cpp). */
	const AliroError error = AliroUd::Lifecycle::RunMutation(
		[&]() -> AliroError { return Aliro::UserDeviceStack::Instance().DeleteCredential(handle); });

	if (error != ALIRO_NO_ERROR) {
		PrintError(sh, "credential delete", error);
		return 0;
	}

	shell_print(sh, "OK");
	return 0;
}

int CmdCredentialReset(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	const AliroError error = AliroUd::Lifecycle::RunMutation(
		[]() -> AliroError { return Aliro::UserDeviceStack::Instance().ResetProvisionedData(); });

	if (error != ALIRO_NO_ERROR) {
		PrintError(sh, "credential reset", error);
		return 0;
	}

	shell_print(sh, "OK");
	return 0;
}

int CmdCredentialBindings(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);

	::Aliro::UserDevice::CredentialHandle handle{};
	if (!ParseHandle(argv[1], handle)) {
		shell_print(sh, "ERR INVALID_ARGUMENT command=credential bindings");
		return 0;
	}

	size_t count{ 0 };
	auto error = Aliro::UserDeviceStack::Instance().GetCredentialGroupBindingCount(handle, count);
	if (error != ALIRO_NO_ERROR) {
		PrintError(sh, "credential bindings", error);
		return 0;
	}

	shell_fprintf(sh, SHELL_NORMAL, "OK count=%zu bindings=", count);
	for (size_t i = 0; i < count; ++i) {
		::Aliro::UserDevice::ReaderGroupIdentifier identifier{};
		error = Aliro::UserDeviceStack::Instance().GetCredentialGroupBinding(handle, i, identifier);
		if (error != ALIRO_NO_ERROR) {
			shell_fprintf(sh, SHELL_NORMAL, "<error>");
			break;
		}

		for (uint8_t byte : identifier) {
			shell_fprintf(sh, SHELL_NORMAL, "%02x", byte);
		}
		if (i + 1 < count) {
			shell_fprintf(sh, SHELL_NORMAL, ",");
		}
	}
	shell_fprintf(sh, SHELL_NORMAL, "\n");
	return 0;
}

int CmdCredentialPreferredSet(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);

	::Aliro::UserDevice::ReaderGroupIdentifier identifier{};
	::Aliro::UserDevice::CredentialHandle handle{};
	if (!ParseHexArray(argv[1], identifier) || !ParseHandle(argv[2], handle)) {
		shell_print(sh, "ERR INVALID_ARGUMENT command=credential preferred-set");
		return 0;
	}

	const auto error = AliroUd::Credential::Store::SetPreferredCredential(identifier, handle);
	if (error != ALIRO_NO_ERROR) {
		PrintError(sh, "credential preferred-set", error);
		return 0;
	}

	shell_print(sh, "OK");
	return 0;
}

int CmdCredentialPreferredGet(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);

	::Aliro::UserDevice::ReaderGroupIdentifier identifier{};
	if (!ParseHexArray(argv[1], identifier)) {
		shell_print(sh, "ERR INVALID_ARGUMENT command=credential preferred-get");
		return 0;
	}

	::Aliro::UserDevice::CredentialHandle handle{};
	const auto error = AliroUd::Credential::Store::GetPreferredCredential(identifier, handle);
	if (error != ALIRO_NO_ERROR) {
		shell_print(sh, "ERR NOT_SET command=credential preferred-get");
		return 0;
	}

	shell_print(sh, "OK handle=%u", static_cast<unsigned>(handle));
	return 0;
}

/*
 * "aliro-ud mailbox" (APP_PLAN.md AWP6): Credential-Issuer-level
 * inspection/read/initialization/reset commands over a credential's
 * mailbox byte storage. These bypass the Reader-facing
 * `MailboxPermissions` (readable/writable) bits entirely (Aliro 1.0
 * Specification section 8.3.1.15, page 60: "The mailbox content SHALL be
 * readable and writeable by the Credential Issuer"); the Reader-gated
 * snapshot/stage/commit session contract is exercised only through the
 * real NFC/stack path, not this CLI.
 */
int CmdMailboxInspect(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);

	::Aliro::UserDevice::CredentialHandle credentialHandle{};
	if (!ParseHandle(argv[1], credentialHandle)) {
		shell_print(sh, "ERR INVALID_ARGUMENT command=mailbox inspect");
		return 0;
	}

	AliroUd::Mailbox::Store::Config config{};
	const auto error = AliroUd::Mailbox::Store::GetConfig(credentialHandle, config);
	if (error != ALIRO_NO_ERROR) {
		PrintError(sh, "mailbox inspect", error);
		return 0;
	}

	/*
	 * WP7 stack impact (see docs/wp7_stack_impact.md): "settable_in_auth1" is
	 * gone (removed upstream); "data_subset_configured"/"data_subset_pairs"
	 * report the AUTH1 mailbox_data_subset descriptor staged by
	 * "credential set-mailbox-data-subset" instead.
	 */
	shell_print(sh,
		    "OK handle=%u size=%u readable=%u writable=%u data_subset_configured=%u data_subset_pairs=%u "
		    "initialized=%u has_data=%u",
		    static_cast<unsigned>(credentialHandle), config.mSizeBytes, config.mPermissions.mReadable ? 1U : 0U,
		    config.mPermissions.mWritable ? 1U : 0U, config.mDataSubsetConfigured ? 1U : 0U,
		    static_cast<unsigned>(config.mDataSubsetPairCount),
		    AliroUd::Mailbox::Store::IsInitialized(credentialHandle) ? 1U : 0U,
		    AliroUd::Mailbox::Store::HasNonZeroData(credentialHandle) ? 1U : 0U);
	return 0;
}

int CmdMailboxRead(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);

	::Aliro::UserDevice::CredentialHandle handle{};
	unsigned long offset{};
	unsigned long length{};
	if (!ParseHandle(argv[1], handle) || !ParseUint(argv[2], offset) || !ParseUint(argv[3], length) ||
	    length > AliroUd::Mailbox::kMaxSizeBytes) {
		shell_print(sh, "ERR INVALID_ARGUMENT command=mailbox read");
		return 0;
	}

	std::array<uint8_t, AliroUd::Mailbox::kMaxSizeBytes> data{};
	const auto error = AliroUd::Mailbox::Store::RawRead(handle, static_cast<size_t>(offset), data.data(),
							    static_cast<size_t>(length));
	if (error != ALIRO_NO_ERROR) {
		PrintError(sh, "mailbox read", error);
		return 0;
	}

	shell_fprintf(sh, SHELL_NORMAL, "OK data=");
	for (size_t i = 0; i < length; ++i) {
		shell_fprintf(sh, SHELL_NORMAL, "%02x", data[i]);
	}
	shell_fprintf(sh, SHELL_NORMAL, "\n");
	return 0;
}

int CmdMailboxInit(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);

	::Aliro::UserDevice::CredentialHandle handle{};
	if (!ParseHandle(argv[1], handle)) {
		shell_print(sh, "ERR INVALID_ARGUMENT command=mailbox init");
		return 0;
	}

	const auto error = AliroUd::Mailbox::Store::Initialize(handle);
	if (error != ALIRO_NO_ERROR) {
		PrintError(sh, "mailbox init", error);
		return 0;
	}

	shell_print(sh, "OK");
	return 0;
}

/*
 * "aliro-ud timing" (APP_PLAN.md AWP7): command-to-response duration
 * evidence. `mEnabled=0` in `stats`' output means
 * CONFIG_ALIRO_UD_TIMING_INSTRUMENTATION is disabled for this build (see
 * command_timing.h); every other field is then always zero.
 */
int CmdTimingStats(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	const auto snapshot = AliroUd::Nfc::GetCommandTimingSnapshot();
	shell_print(sh, "OK enabled=%u samples=%zu last_ms=%u max_ms=%u", snapshot.mEnabled ? 1U : 0U,
		    snapshot.mSampleCount, snapshot.mLastDurationMs, snapshot.mMaxDurationMs);
	return 0;
}

int CmdTimingReset(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	AliroUd::Nfc::ResetCommandTimingStats();
	shell_print(sh, "OK");
	return 0;
}

int CmdMailboxReset(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);

	::Aliro::UserDevice::CredentialHandle handle{};
	if (!ParseHandle(argv[1], handle)) {
		shell_print(sh, "ERR INVALID_ARGUMENT command=mailbox reset");
		return 0;
	}

	const auto error = AliroUd::Mailbox::Store::Reset(handle);
	if (error != ALIRO_NO_ERROR) {
		PrintError(sh, "mailbox reset", error);
		return 0;
	}

	shell_print(sh, "OK");
	return 0;
}

/*
 * "aliro-ud document": inspection of committed Access/Revocation Documents.
 * Documents are presented to Readers in the step-up phase (Aliro 1.0
 * Specification, section 7.1, page 32) and are not secret. The CLI shell
 * thread is the only caller, which serializes use of sDocumentRecord.
 */
AliroUd::Credential::PersistedCredential sDocumentRecord{};

int CmdDocumentInspect(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);

	::Aliro::UserDevice::CredentialHandle handle{};
	if (!ParseHandle(argv[1], handle)) {
		shell_print(sh, "ERR INVALID_ARGUMENT command=document inspect");
		return 0;
	}

	const auto error = AliroUd::Credential::Store::GetFullRecord(handle, sDocumentRecord);
	if (error != ALIRO_NO_ERROR) {
		PrintError(sh, "document inspect", error);
		return 0;
	}

	shell_print(sh, "OK handle=%u access=%u access_length=%u revocation=%u revocation_length=%u",
		    static_cast<unsigned>(handle), sDocumentRecord.mAccessDocument.mPresent ? 1U : 0U,
		    static_cast<unsigned>(sDocumentRecord.mAccessDocument.mLength),
		    sDocumentRecord.mRevocationDocument.mPresent ? 1U : 0U,
		    static_cast<unsigned>(sDocumentRecord.mRevocationDocument.mLength));
	return 0;
}

int CmdDocumentRead(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);

	::Aliro::UserDevice::CredentialHandle handle{};
	::Aliro::AccessDocumentTypes::DocumentType type{};
	unsigned long offset{};
	unsigned long length{};
	if (!ParseHandle(argv[1], handle) || !ParseDocumentType(argv[2], type) || !ParseUint(argv[3], offset) ||
	    !ParseUint(argv[4], length)) {
		shell_print(sh, "ERR INVALID_ARGUMENT command=document read");
		return 0;
	}

	const auto error = AliroUd::Credential::Store::GetFullRecord(handle, sDocumentRecord);
	if (error != ALIRO_NO_ERROR) {
		PrintError(sh, "document read", error);
		return 0;
	}

	const auto &document = (type == ::Aliro::AccessDocumentTypes::DocumentType::Access)
				       ? sDocumentRecord.mAccessDocument
				       : sDocumentRecord.mRevocationDocument;
	if (!document.mPresent) {
		shell_print(sh, "ERR NOT_PRESENT command=document read");
		return 0;
	}

	if (offset > document.mLength || length > document.mLength - offset) {
		shell_print(sh, "ERR INVALID_ARGUMENT command=document read");
		return 0;
	}

	shell_fprintf(sh, SHELL_NORMAL, "OK data=");
	for (size_t i = 0; i < length; ++i) {
		shell_fprintf(sh, SHELL_NORMAL, "%02x", document.mData[offset + i]);
	}
	shell_fprintf(sh, SHELL_NORMAL, "\n");
	return 0;
}

/*
 * One in-memory staging transaction (APP_PLAN.md AWP3): begin-create/
 * begin-update open it, the field setters below mutate it, and commit/abort
 * close it. None of them take a credential handle argument except
 * begin-update (which credential to clone); AWP3 owns the transaction state
 * itself.
 */
SHELL_STATIC_SUBCMD_SET_CREATE(
	sub_credential,
	SHELL_CMD_ARG(begin-create, NULL, "Start staging a new Access Credential.", CmdCredentialBeginCreate, 1,
		      0),
	SHELL_CMD_ARG(begin-update, NULL,
		      "<handle> Start staging an update to an existing Access Credential.",
		      CmdCredentialBeginUpdate, 2, 0),
	SHELL_CMD_ARG(set-key, NULL, "<hex> Stage the raw Access Credential private-key input.", CmdCredentialSetKey,
		      2, 0),
	SHELL_CMD_ARG(set-binding, NULL,
		      "<index> <reader_group_identifier_hex> <direct|issuer> <key_hex> Stage one "
		      "reader_group_identifier binding.",
		      CmdCredentialSetBinding, 5, 0),
	SHELL_CMD_ARG(set-policy, NULL, "<1|2|3> Stage the authentication_policy.", CmdCredentialSetPolicy, 2, 0),
	SHELL_CMD_ARG(set-mailbox, NULL, "<size> <rights (0-3: bit0=readable, bit1=writable)> Stage the mailbox configuration.",
		      CmdCredentialSetMailbox, 3, 0),
	SHELL_CMD_ARG(set-mailbox-data-subset, NULL,
		      "<index> <offset> <length> Stage one AUTH1 mailbox_data_subset pair.",
		      CmdCredentialSetMailboxDataSubset, 4, 0),
	SHELL_CMD_ARG(set-credential-timestamp, NULL, "<hex> Stage credential_signed_timestamp.",
		      CmdCredentialSetCredentialTimestamp, 2, 0),
	SHELL_CMD_ARG(set-revocation-timestamp, NULL, "<hex> Stage revocation_signed_timestamp.",
		      CmdCredentialSetRevocationTimestamp, 2, 0),
	SHELL_CMD_ARG(set-document, NULL,
		      "<access|revocation> <length> <hex> Stage an Access or Revocation Document of <length> bytes.",
		      CmdCredentialSetDocument, 4, 0),
	SHELL_CMD_ARG(clear-document, NULL, "<access|revocation> Stage removal of an Access or Revocation Document.",
		      CmdCredentialClearDocument, 2, 0),
	SHELL_CMD_ARG(commit, NULL, "Validate and persist the staged candidate.", CmdCredentialCommit, 1, 0),
	SHELL_CMD_ARG(abort, NULL, "Discard the staged candidate.", CmdCredentialAbort, 1, 0),
	SHELL_CMD_ARG(inspect, NULL, "<handle> Report non-secret metadata for a credential.", CmdCredentialInspect,
		      2, 0),
	SHELL_CMD_ARG(list, NULL, "List every provisioned credential's non-secret metadata.", CmdCredentialList, 1,
		      0),
	SHELL_CMD_ARG(delete, NULL, "<handle> Delete a credential.", CmdCredentialDelete, 2, 0),
	SHELL_CMD_ARG(reset, NULL, "Factory-reset every provisioned credential.", CmdCredentialReset, 1, 0),
	SHELL_CMD_ARG(bindings, NULL, "<handle> Enumerate a credential's reader_group_identifier bindings.",
		      CmdCredentialBindings, 2, 0),
	SHELL_CMD_ARG(preferred-set, NULL,
		      "<reader_group_identifier_hex> <handle> Set the preferred credential for a shared "
		      "reader_group_identifier.",
		      CmdCredentialPreferredSet, 3, 0),
	SHELL_CMD_ARG(preferred-get, NULL, "<reader_group_identifier_hex> Get the preferred credential, if any.",
		      CmdCredentialPreferredGet, 2, 0),
	SHELL_SUBCMD_SET_END);

SHELL_STATIC_SUBCMD_SET_CREATE(
	sub_auth,
	SHELL_CMD_ARG(status, NULL, "Report the current button authorization window state.", CmdAuthStatus, 1, 0),
	SHELL_CMD_ARG(press, NULL,
		      "Test trigger: open the authorization window as if the DK button were pressed.",
		      CmdAuthPress, 1, 0),
	SHELL_CMD_ARG(clear, NULL, "Test trigger: immediately close the authorization window.", CmdAuthClear, 1,
		      0),
	SHELL_CMD_ARG(notify-required, NULL,
		      "Test trigger: invoke NotifyAuthenticationRequired() as the stack would for an "
		      "AUTH0 policy 1-3 credential with no valid window (lights the LED).",
		      CmdAuthNotifyRequired, 1, 0),
	SHELL_SUBCMD_SET_END);

SHELL_STATIC_SUBCMD_SET_CREATE(
	sub_mailbox,
	SHELL_CMD_ARG(inspect, NULL, "<handle> Report non-secret mailbox configuration/state for a credential.",
		      CmdMailboxInspect, 2, 0),
	SHELL_CMD_ARG(read, NULL, "<handle> <offset> <length> Read committed mailbox bytes (hex).", CmdMailboxRead,
		      4, 0),
	SHELL_CMD_ARG(init, NULL,
		      "<handle> Ensure committed mailbox byte storage exists (idempotent, zero-fills on first use).",
		      CmdMailboxInit, 2, 0),
	SHELL_CMD_ARG(reset, NULL, "<handle> Explicitly re-zero a mailbox's committed byte storage.",
		      CmdMailboxReset, 2, 0),
	SHELL_SUBCMD_SET_END);

SHELL_STATIC_SUBCMD_SET_CREATE(
	sub_document,
	SHELL_CMD_ARG(inspect, NULL, "<handle> Report which documents a credential has, and their lengths.",
		      CmdDocumentInspect, 2, 0),
	SHELL_CMD_ARG(read, NULL, "<handle> <access|revocation> <offset> <length> Read committed document bytes (hex).",
		      CmdDocumentRead, 5, 0),
	SHELL_SUBCMD_SET_END);

SHELL_STATIC_SUBCMD_SET_CREATE(
	sub_timing,
	SHELL_CMD_ARG(stats, NULL,
		      "Report command-to-response timing evidence (sample count, last/max duration).",
		      CmdTimingStats, 1, 0),
	SHELL_CMD_ARG(reset, NULL, "Reset command-to-response timing statistics.", CmdTimingReset, 1, 0),
	SHELL_SUBCMD_SET_END);

SHELL_STATIC_SUBCMD_SET_CREATE(
	sub_aliro_ud,
	SHELL_CMD_ARG(info, NULL, "Report non-secret build/initialization/session state.", CmdInfo, 1, 0),
	SHELL_CMD(credential, &sub_credential, "Access Credential provisioning/staging commands (AWP3).", NULL),
	SHELL_CMD(auth, &sub_auth, "Button authorization window status/test-trigger commands (AWP4).", NULL),
	SHELL_CMD(mailbox, &sub_mailbox, "Mailbox inspection/read/initialization/reset commands (AWP6).", NULL),
	SHELL_CMD(document, &sub_document, "Access/Revocation Document inspection commands.", NULL),
	SHELL_CMD(timing, &sub_timing, "Command-to-response timing evidence commands (AWP7).", NULL),
	SHELL_SUBCMD_SET_END);

/*
 * Registered by hand (mirroring SHELL_CMD_ARG_REGISTER) instead of through
 * that macro directly: the macro token-pastes `syntax` into the generated
 * variable names, which is not possible for the hyphenated command name
 * APP_PLAN.md AWP2 specifies ("aliro-ud"). `STRINGIFY()` inside
 * `SHELL_CMD_ARG()` below only turns `aliro-ud` into the text "aliro-ud", so
 * that part works unmodified.
 */
static const struct shell_static_entry sRootEntry =
	SHELL_CMD_ARG(aliro-ud, &sub_aliro_ud, "Aliro User Device commands.", NULL, 1, 0);

static const TYPE_SECTION_ITERABLE(union shell_cmd_entry, sRootCmd, shell_root_cmds,
				    sRootCmd) = { .entry = &sRootEntry };

} // namespace
} // namespace AliroUd::Cli
