/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include <zephyr/kernel.h>
#include <zephyr/shell/shell.h>
#include <zephyr/shell/shell_dummy.h>
#include <zephyr/ztest.h>

#include "platform/nfc/nfc_worker.h"

#include <app_version.h>

#include <cstring>
#include <string>

/**
 * @brief Exercises the "aliro-ud" development CLI (APP_PLAN.md AWP2/AWP3)
 * through Zephyr's dummy shell backend, in front of the real
 * `platform/nfc`/`platform/os` code and the real Aliro `UserDeviceStack`
 * (same substitution as `worker_lifecycle`: only the hardware-dependent
 * `nfc_transport.cpp` is replaced by a fake). Credential persistence uses
 * the in-memory fakes from `tests/.../common/` rather than real flash/PSA
 * storage, but `fake_key_backend.cpp` performs real PSA volatile crypto so
 * the private-key scalar validation exercised here is cryptographically
 * real.
 */
namespace {

void SettleWorker()
{
	k_msleep(50);
}

void *SetupCli(void)
{
	const struct shell *sh = shell_backend_dummy_get_ptr();

	WAIT_FOR(shell_ready(sh), 20000, k_msleep(1));
	zassert_true(shell_ready(sh), "timed out waiting for dummy shell backend");

	AliroUd::Nfc::StartWorker();
	SettleWorker();
	return nullptr;
}

std::string RunCommand(const char *cmd)
{
	const struct shell *sh = shell_backend_dummy_get_ptr();

	shell_backend_dummy_clear_output(sh);
	(void)shell_execute_cmd(sh, cmd);

	size_t size{ 0 };
	const char *buf = shell_backend_dummy_get_output(sh, &size);
	return std::string(buf, size);
}

} // namespace

ZTEST_SUITE(aliro_ud_cli_info, nullptr, SetupCli, nullptr, nullptr, nullptr);

/**
 * @brief `aliro-ud info` reports one deterministic "OK ..." line carrying
 * the current build version, initialization state, and NFC session
 * diagnostics, matching APP_PLAN.md AWP2's "info" contract.
 */
ZTEST(aliro_ud_cli_info, test_info_reports_ok_line)
{
	const std::string output{ RunCommand("aliro-ud info") };

	zassert_true(output.find("OK ") != std::string::npos, "expected an 'OK' line, got: %s", output.c_str());
	zassert_true(output.find("version=" APP_VERSION_EXTENDED_STRING) != std::string::npos,
		     "expected the build version, got: %s", output.c_str());
	/*
	 * This test never calls AliroUd::AppStatus::SetInitState() (that is
	 * main()'s job, APP_PLAN.md AWP1/AWP2); "info" must still report a
	 * valid, non-secret token rather than crash or omit the field.
	 */
	zassert_true(output.find("init=not_started") != std::string::npos, "expected init=not_started, got: %s",
		     output.c_str());
	zassert_true(output.find("session_active=0") != std::string::npos, "expected session_active=0, got: %s",
		     output.c_str());
	zassert_true(output.find("activation_attempts=") != std::string::npos,
		     "expected an activation_attempts field, got: %s", output.c_str());
	zassert_true(output.find("rejected_apdus=") != std::string::npos,
		     "expected a rejected_apdus field, got: %s", output.c_str());
}

/**
 * @brief `info` reflects real, live NFC worker diagnostics (not a fixed
 * canned string): after a field activation the session is reported active
 * and the activation counter advances.
 */
ZTEST(aliro_ud_cli_info, test_info_reflects_live_session_state)
{
	AliroUd::Nfc::PostFieldOn();
	SettleWorker();

	const std::string output{ RunCommand("aliro-ud info") };

	zassert_true(output.find("session_active=1") != std::string::npos, "expected session_active=1, got: %s",
		     output.c_str());
	zassert_true(output.find("activation_attempts=1") != std::string::npos,
		     "expected activation_attempts=1, got: %s", output.c_str());

	AliroUd::Nfc::PostFieldOff();
	SettleWorker();
}

/**
 * @brief A staging transaction with no `begin-create`/`begin-update` open
 * is rejected deterministically (APP_PLAN.md AWP3): every setter and
 * `commit` require an active transaction, and `abort` requires one too.
 */
ZTEST(aliro_ud_cli_info, test_credential_setters_require_open_transaction)
{
	static const char *const kCommands[]{
		"aliro-ud credential set-key 00",
		"aliro-ud credential set-binding 0 00 direct 00",
		"aliro-ud credential set-policy 1",
		"aliro-ud credential set-mailbox 512 3",
		"aliro-ud credential set-mailbox-data-subset 0 0 1",
		"aliro-ud credential set-credential-timestamp 00",
		"aliro-ud credential set-revocation-timestamp 00",
		"aliro-ud credential commit",
		"aliro-ud credential abort",
	};

	for (const char *cmd : kCommands) {
		const std::string output{ RunCommand(cmd) };
		zassert_true(output.find("ERR NO_TRANSACTION") != std::string::npos,
			     "expected 'ERR NO_TRANSACTION' for '%s', got: %s", cmd, output.c_str());
	}
}

/**
 * @brief End-to-end staging transaction (APP_PLAN.md AWP3): `begin-create`,
 * a full set of setters, and `commit` persist a real Access Credential
 * whose non-secret metadata is then visible through `inspect`/`list`, and
 * `delete` removes it again.
 */
ZTEST(aliro_ud_cli_info, test_credential_staging_transaction_commits)
{
	/* An arbitrary valid (nonzero, below the P-256 curve order) private key scalar. */
	static const char *const kKeyHex{ "23231022a3662ceb6f2e6a4e998866ae88d6e9da1c72b050ae5c206a1da46712" };
	static const char *const kReaderGroupIdentifierHex{ "0102030405060708090a0b0c0d0e0f10" };
	static const char *const kTrustAnchorKeyHex{
		"04"
		"1111111111111111111111111111111111111111111111111111111111111111"
		"2222222222222222222222222222222222222222222222222222222222222222"
	};

	zassert_true(RunCommand("aliro-ud credential begin-create").find("OK") != std::string::npos,
		     "begin-create should succeed");
	zassert_true(RunCommand((std::string("aliro-ud credential set-key ") + kKeyHex).c_str()).find("OK") !=
			     std::string::npos,
		     "set-key should accept a valid P-256 scalar");
	zassert_true(RunCommand("aliro-ud credential set-policy 1").find("OK") != std::string::npos,
		     "set-policy should succeed");
	zassert_true(RunCommand((std::string("aliro-ud credential set-binding 0 ") + kReaderGroupIdentifierHex +
				  " direct " + kTrustAnchorKeyHex)
					 .c_str())
			     .find("OK") != std::string::npos,
		     "set-binding should succeed");
	zassert_true(RunCommand("aliro-ud credential set-mailbox 8 3").find("OK") != std::string::npos,
		     "set-mailbox should succeed");

	const std::string commitOutput{ RunCommand("aliro-ud credential commit") };
	zassert_true(commitOutput.find("OK handle=") != std::string::npos, "expected 'OK handle=', got: %s",
		     commitOutput.c_str());

	const std::string inspectOutput{ RunCommand("aliro-ud credential inspect 1") };
	zassert_true(inspectOutput.find("OK handle=1") != std::string::npos, "expected handle=1, got: %s",
		     inspectOutput.c_str());
	zassert_true(inspectOutput.find("bindings=1") != std::string::npos, "expected bindings=1, got: %s",
		     inspectOutput.c_str());

	const std::string listOutput{ RunCommand("aliro-ud credential list") };
	zassert_true(listOutput.find("OK count=1") != std::string::npos, "expected count=1, got: %s",
		     listOutput.c_str());

	zassert_true(RunCommand("aliro-ud credential delete 1").find("OK") != std::string::npos,
		     "delete should succeed");

	const std::string listAfterDelete{ RunCommand("aliro-ud credential list") };
	zassert_true(listAfterDelete.find("OK count=0") != std::string::npos, "expected count=0, got: %s",
		     listAfterDelete.c_str());
}

/**
 * @brief "aliro-ud mailbox" (APP_PLAN.md AWP6): "inspect" reports the
 * committed configuration staged by "credential set-mailbox", "init"
 * lazily zero-fills the committed byte storage, "read" returns the
 * committed bytes, and deleting the owning credential erases the mailbox
 * (no stale data survives handle reuse).
 */
ZTEST(aliro_ud_cli_info, test_mailbox_inspect_init_read_and_erase_on_delete)
{
	static const char *const kKeyHex{ "23231022a3662ceb6f2e6a4e998866ae88d6e9da1c72b050ae5c206a1da46712" };
	static const char *const kReaderGroupIdentifierHex{ "0102030405060708090a0b0c0d0e0f10" };
	static const char *const kTrustAnchorKeyHex{
		"04"
		"1111111111111111111111111111111111111111111111111111111111111111"
		"2222222222222222222222222222222222222222222222222222222222222222"
	};

	zassert_true(RunCommand("aliro-ud credential begin-create").find("OK") != std::string::npos,
		     "begin-create should succeed");
	RunCommand((std::string("aliro-ud credential set-key ") + kKeyHex).c_str());
	RunCommand("aliro-ud credential set-policy 1");
	RunCommand((std::string("aliro-ud credential set-binding 0 ") + kReaderGroupIdentifierHex + " direct " +
		    kTrustAnchorKeyHex)
			   .c_str());
	zassert_true(RunCommand("aliro-ud credential set-mailbox 8 3").find("OK") != std::string::npos,
		     "set-mailbox should succeed");
	zassert_true(RunCommand("aliro-ud credential commit").find("OK handle=") != std::string::npos,
		     "commit should succeed");

	/*
	 * WP7 stack impact (see docs/wp7_stack_impact.md): "settable_in_auth1"
	 * is gone; "data_subset_configured"/"data_subset_pairs" replace it.
	 */
	const std::string inspectBeforeInit{ RunCommand("aliro-ud mailbox inspect 1") };
	zassert_true(inspectBeforeInit.find("OK handle=1 size=8 readable=1 writable=1 data_subset_configured=0 "
					     "data_subset_pairs=0") != std::string::npos,
		     "expected the staged mailbox configuration, got: %s", inspectBeforeInit.c_str());
	zassert_true(inspectBeforeInit.find("initialized=0") != std::string::npos,
		     "mailbox should not be initialized before 'mailbox init', got: %s",
		     inspectBeforeInit.c_str());

	zassert_true(RunCommand("aliro-ud mailbox init 1").find("OK") != std::string::npos,
		     "mailbox init should succeed");

	const std::string inspectAfterInit{ RunCommand("aliro-ud mailbox inspect 1") };
	zassert_true(inspectAfterInit.find("initialized=1") != std::string::npos,
		     "expected initialized=1 after 'mailbox init', got: %s", inspectAfterInit.c_str());
	zassert_true(inspectAfterInit.find("has_data=0") != std::string::npos,
		     "a freshly initialized mailbox should be all-zero, got: %s", inspectAfterInit.c_str());

	const std::string readOutput{ RunCommand("aliro-ud mailbox read 1 0 8") };
	zassert_true(readOutput.find("OK data=0000000000000000") != std::string::npos,
		     "expected 8 zero bytes, got: %s", readOutput.c_str());

	zassert_true(RunCommand("aliro-ud credential delete 1").find("OK") != std::string::npos,
		     "delete should succeed");

	const std::string inspectAfterDelete{ RunCommand("aliro-ud mailbox inspect 1") };
	zassert_true(inspectAfterDelete.find("ERR") != std::string::npos,
		     "inspecting a deleted credential's mailbox should fail, got: %s",
		     inspectAfterDelete.c_str());
}

/**
 * @brief "credential set-mailbox-data-subset" (WP7 stack impact; see
 * docs/wp7_stack_impact.md) stages an AUTH1 mailbox_data_subset (offset,
 * length) pair, visible through "mailbox inspect" once committed.
 */
ZTEST(aliro_ud_cli_info, test_mailbox_data_subset_staging_and_inspect)
{
	static const char *const kKeyHex{ "23231022a3662ceb6f2e6a4e998866ae88d6e9da1c72b050ae5c206a1da46712" };
	static const char *const kReaderGroupIdentifierHex{ "0102030405060708090a0b0c0d0e0f10" };
	static const char *const kTrustAnchorKeyHex{
		"04"
		"1111111111111111111111111111111111111111111111111111111111111111"
		"2222222222222222222222222222222222222222222222222222222222222222"
	};

	RunCommand("aliro-ud credential begin-create");
	RunCommand((std::string("aliro-ud credential set-key ") + kKeyHex).c_str());
	RunCommand("aliro-ud credential set-policy 1");
	RunCommand((std::string("aliro-ud credential set-binding 0 ") + kReaderGroupIdentifierHex + " direct " +
		    kTrustAnchorKeyHex)
			   .c_str());
	zassert_true(RunCommand("aliro-ud credential set-mailbox 8 3").find("OK") != std::string::npos,
		     "set-mailbox should succeed");
	zassert_true(RunCommand("aliro-ud credential set-mailbox-data-subset 0 2 4").find("OK") != std::string::npos,
		     "set-mailbox-data-subset should succeed for a pair within the mailbox's provisioned size");
	zassert_true(RunCommand("aliro-ud credential commit").find("OK handle=") != std::string::npos,
		     "commit should succeed");

	const std::string inspectOutput{ RunCommand("aliro-ud mailbox inspect 1") };
	zassert_true(inspectOutput.find("data_subset_configured=1 data_subset_pairs=1") != std::string::npos,
		     "expected one configured mailbox_data_subset pair, got: %s", inspectOutput.c_str());
}

/**
 * @brief "aliro-ud auth" (APP_PLAN.md AWP4): "status" reports the live
 * button-authorization window state, "press" is the application test
 * trigger that opens it without physical DK hardware, and "clear" revokes
 * it again.
 */
ZTEST(aliro_ud_cli_info, test_auth_status_press_clear)
{
	zassert_true(RunCommand("aliro-ud auth clear").find("OK") != std::string::npos, "clear should succeed");

	const std::string beforePress{ RunCommand("aliro-ud auth status") };
	zassert_true(beforePress.find("OK state=required") != std::string::npos,
		     "expected state=required before any press, got: %s", beforePress.c_str());

	zassert_true(RunCommand("aliro-ud auth press").find("OK") != std::string::npos, "press should succeed");

	const std::string afterPress{ RunCommand("aliro-ud auth status") };
	zassert_true(afterPress.find("OK state=authorized") != std::string::npos,
		     "expected state=authorized after press, got: %s", afterPress.c_str());

	zassert_true(RunCommand("aliro-ud auth clear").find("OK") != std::string::npos, "clear should succeed");

	const std::string afterClear{ RunCommand("aliro-ud auth status") };
	zassert_true(afterClear.find("OK state=required") != std::string::npos,
		     "expected state=required after clear, got: %s", afterClear.c_str());
}
