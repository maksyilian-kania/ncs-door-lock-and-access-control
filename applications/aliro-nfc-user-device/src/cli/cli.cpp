/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include <app_version.h>
#include <zephyr/shell/shell.h>
#include <zephyr/sys/iterable_sections.h>
#include <zephyr/sys/util.h>

#include "platform/nfc/nfc_worker.h"
#include "platform/os/app_status.h"

/*
 * Development CLI over the DK virtual UART (APP_PLAN.md AWP2): a Zephyr
 * shell "aliro-ud" root command with an "info" command reporting non-secret
 * build/initialization/session state, and an "aliro-ud credential" group of
 * staging-transaction command shells. Every handler here prints exactly one
 * deterministic, machine-readable "OK ..."/"ERR ..." line and never a secret
 * value; the credential staging shells are syntax/argument-shape only for
 * now — AWP3 (APP_PLAN.md) supplies their storage behavior. Inspection,
 * deletion, selection, trust-binding, document, and mailbox subcommands are
 * intentionally not added yet; they are reserved for AWP3/AWP6.
 */
namespace AliroUd::Cli {
namespace {

void PrintNotImplemented(const struct shell *sh, const char *command)
{
	shell_print(sh, "ERR NOT_IMPLEMENTED command=%s", command);
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

int CmdCredentialBeginCreate(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	PrintNotImplemented(sh, "credential begin-create");
	return 0;
}

int CmdCredentialBeginUpdate(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	PrintNotImplemented(sh, "credential begin-update");
	return 0;
}

int CmdCredentialSetKey(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	PrintNotImplemented(sh, "credential set-key");
	return 0;
}

int CmdCredentialSetBinding(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	PrintNotImplemented(sh, "credential set-binding");
	return 0;
}

int CmdCredentialSetPolicy(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	PrintNotImplemented(sh, "credential set-policy");
	return 0;
}

int CmdCredentialSetMailbox(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	PrintNotImplemented(sh, "credential set-mailbox");
	return 0;
}

int CmdCredentialSetCredentialTimestamp(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	PrintNotImplemented(sh, "credential set-credential-timestamp");
	return 0;
}

int CmdCredentialSetRevocationTimestamp(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	PrintNotImplemented(sh, "credential set-revocation-timestamp");
	return 0;
}

int CmdCredentialCommit(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	PrintNotImplemented(sh, "credential commit");
	return 0;
}

int CmdCredentialAbort(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	PrintNotImplemented(sh, "credential abort");
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
	SHELL_CMD_ARG(commit, NULL, "Validate and persist the staged candidate.", CmdCredentialCommit, 1, 0),
	SHELL_CMD_ARG(abort, NULL, "Discard the staged candidate.", CmdCredentialAbort, 1, 0), SHELL_SUBCMD_SET_END);

SHELL_STATIC_SUBCMD_SET_CREATE(
	sub_aliro_ud,
	SHELL_CMD_ARG(info, NULL, "Report non-secret build/initialization/session state.", CmdInfo, 1, 0),
	SHELL_CMD(credential, &sub_credential, "Access Credential staging commands (AWP3 supplies behavior).",
		  NULL),
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
