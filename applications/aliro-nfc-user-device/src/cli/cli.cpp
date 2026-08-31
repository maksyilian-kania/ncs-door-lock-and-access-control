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
#include "platform/nfc/nfc_worker.h"
#include "platform/os/app_status.h"
#include "storage/credential/credential_store.h"
#include "storage/credential/credential_types.h"
#include "storage/credential/provisioning.h"

#include <aliro/user_device/interface.h>
#include <aliro/user_device/user_device.h>

#include <cstdlib>

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

bool ParseHexBytes(const char *hex, uint8_t *out, size_t outLen)
{
	if (strlen(hex) != outLen * 2) {
		return false;
	}

	for (size_t i = 0; i < outLen; ++i) {
		char byteStr[3]{ hex[2 * i], hex[2 * i + 1], '\0' };
		char *end{ nullptr };
		const unsigned long value = strtoul(byteStr, &end, 16);

		if (end != byteStr + 2) {
			return false;
		}

		out[i] = static_cast<uint8_t>(value);
	}

	return true;
}

template <size_t N> bool ParseHexArray(const char *hex, std::array<uint8_t, N> &out)
{
	return ParseHexBytes(hex, out.data(), N);
}

bool ParseUint(const char *str, unsigned long &out)
{
	char *end{ nullptr };
	out = strtoul(str, &end, 10);
	return end != str && *end == '\0';
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

	unsigned long handle{};
	if (!ParseUint(argv[1], handle)) {
		shell_print(sh, "ERR INVALID_ARGUMENT command=credential begin-update");
		return 0;
	}

	AliroUd::Credential::PersistedCredential record{};
	const auto error =
		AliroUd::Credential::Store::GetFullRecord(static_cast<::Aliro::UserDevice::CredentialHandle>(handle),
							  record);
	if (error != ALIRO_NO_ERROR) {
		PrintError(sh, "credential begin-update", error);
		return 0;
	}

	sCandidate = StagingCandidate{};
	sCandidate.mActive = true;
	sCandidate.mIsUpdate = true;
	sCandidate.mBaseHandle = static_cast<::Aliro::UserDevice::CredentialHandle>(handle);
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
	if (!ParseUint(argv[1], size) || size > kMailboxMaxSizeBytes || !ParseUint(argv[2], rights) || rights > 7) {
		shell_print(sh, "ERR INVALID_ARGUMENT command=credential set-mailbox");
		return 0;
	}

	sCandidate.mMailbox.mConfigured = true;
	sCandidate.mMailbox.mSizeBytes = static_cast<uint32_t>(size);
	sCandidate.mMailbox.mReadable = (rights & 0x1) != 0;
	sCandidate.mMailbox.mWritable = (rights & 0x2) != 0;
	sCandidate.mMailbox.mSettableInAuth1 = (rights & 0x4) != 0;
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

int SetDocument(const struct shell *sh, const char *command, const char *hex, AliroUd::Credential::OptionalDocument &out)
{
	if (!sCandidate.mActive) {
		shell_print(sh, "ERR NO_TRANSACTION command=%s", command);
		return 0;
	}

	const size_t hexLen = strlen(hex);
	if (hexLen % 2 != 0 || hexLen / 2 > kDocumentMaxSizeBytes) {
		shell_print(sh, "ERR INVALID_ARGUMENT command=%s", command);
		return 0;
	}

	AliroUd::Credential::OptionalDocument document{};
	document.mLength = static_cast<uint32_t>(hexLen / 2);
	if (!ParseHexBytes(hex, document.mData.data(), document.mLength)) {
		shell_print(sh, "ERR INVALID_ARGUMENT command=%s", command);
		return 0;
	}
	document.mPresent = true;

	out = document;
	shell_print(sh, "OK");
	return 0;
}

int CmdCredentialSetAccessDocument(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	return SetDocument(sh, "credential set-access-document", argv[1], sCandidate.mAccessDocument);
}

int CmdCredentialSetRevocationDocument(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	return SetDocument(sh, "credential set-revocation-document", argv[1], sCandidate.mRevocationDocument);
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

	unsigned long handle{};
	if (!ParseUint(argv[1], handle)) {
		shell_print(sh, "ERR INVALID_ARGUMENT command=credential inspect");
		return 0;
	}

	::Aliro::UserDevice::CredentialMetadata meta{};
	const auto error = Aliro::UserDeviceStack::Instance().GetCredentialMetadata(
		static_cast<::Aliro::UserDevice::CredentialHandle>(handle), meta);

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

	unsigned long handle{};
	if (!ParseUint(argv[1], handle)) {
		shell_print(sh, "ERR INVALID_ARGUMENT command=credential delete");
		return 0;
	}

	const AliroError error = AliroUd::Lifecycle::RunMutation([&]() -> AliroError {
		return Aliro::UserDeviceStack::Instance().DeleteCredential(
			static_cast<::Aliro::UserDevice::CredentialHandle>(handle));
	});

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

	unsigned long handle{};
	if (!ParseUint(argv[1], handle)) {
		shell_print(sh, "ERR INVALID_ARGUMENT command=credential bindings");
		return 0;
	}

	size_t count{ 0 };
	auto error = Aliro::UserDeviceStack::Instance().GetCredentialGroupBindingCount(
		static_cast<::Aliro::UserDevice::CredentialHandle>(handle), count);
	if (error != ALIRO_NO_ERROR) {
		PrintError(sh, "credential bindings", error);
		return 0;
	}

	shell_fprintf(sh, SHELL_NORMAL, "OK count=%zu bindings=", count);
	for (size_t i = 0; i < count; ++i) {
		::Aliro::UserDevice::ReaderGroupIdentifier identifier{};
		error = Aliro::UserDeviceStack::Instance().GetCredentialGroupBinding(
			static_cast<::Aliro::UserDevice::CredentialHandle>(handle), i, identifier);
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
	unsigned long handle{};
	if (!ParseHexArray(argv[1], identifier) || !ParseUint(argv[2], handle)) {
		shell_print(sh, "ERR INVALID_ARGUMENT command=credential preferred-set");
		return 0;
	}

	const auto error = AliroUd::Credential::Store::SetPreferredCredential(
		identifier, static_cast<::Aliro::UserDevice::CredentialHandle>(handle));
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
	SHELL_CMD_ARG(set-mailbox, NULL, "<size> <rights> Stage the mailbox configuration.",
		      CmdCredentialSetMailbox, 3, 0),
	SHELL_CMD_ARG(set-credential-timestamp, NULL, "<hex> Stage credential_signed_timestamp.",
		      CmdCredentialSetCredentialTimestamp, 2, 0),
	SHELL_CMD_ARG(set-revocation-timestamp, NULL, "<hex> Stage revocation_signed_timestamp.",
		      CmdCredentialSetRevocationTimestamp, 2, 0),
	SHELL_CMD_ARG(set-access-document, NULL, "<hex> Stage the optional Access Document.",
		      CmdCredentialSetAccessDocument, 2, 0),
	SHELL_CMD_ARG(set-revocation-document, NULL, "<hex> Stage the optional Revocation Document.",
		      CmdCredentialSetRevocationDocument, 2, 0),
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
	sub_aliro_ud,
	SHELL_CMD_ARG(info, NULL, "Report non-secret build/initialization/session state.", CmdInfo, 1, 0),
	SHELL_CMD(credential, &sub_credential, "Access Credential provisioning/staging commands (AWP3).", NULL),
	SHELL_CMD(auth, &sub_auth, "Button authorization window status/test-trigger commands (AWP4).", NULL),
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
