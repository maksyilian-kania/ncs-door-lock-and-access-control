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
 * @brief Exercises the "aliro-ud" development CLI (APP_PLAN.md AWP2)
 * through Zephyr's dummy shell backend, in front of the real
 * `platform/nfc`/`platform/os` code and the real Aliro `UserDeviceStack`
 * (same substitution as `worker_lifecycle`: only the hardware-dependent
 * `nfc_transport.cpp` is replaced by a fake).
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
 * @brief Credential staging shells are registered and syntactically usable
 * (APP_PLAN.md AWP2), but every one returns a deterministic "ERR
 * NOT_IMPLEMENTED" line: AWP3 supplies their storage behavior.
 */
ZTEST(aliro_ud_cli_info, test_credential_shells_are_not_yet_implemented)
{
	static const char *const kCommands[]{
		"aliro-ud credential begin-create",
		"aliro-ud credential begin-update 1",
		"aliro-ud credential set-key 00",
		"aliro-ud credential set-binding 0 00 direct 00",
		"aliro-ud credential set-policy 1",
		"aliro-ud credential set-mailbox 512 3",
		"aliro-ud credential set-credential-timestamp 00",
		"aliro-ud credential set-revocation-timestamp 00",
		"aliro-ud credential commit",
		"aliro-ud credential abort",
	};

	for (const char *cmd : kCommands) {
		const std::string output{ RunCommand(cmd) };
		zassert_true(output.find("ERR NOT_IMPLEMENTED") != std::string::npos,
			     "expected 'ERR NOT_IMPLEMENTED' for '%s', got: %s", cmd, output.c_str());
	}
}
