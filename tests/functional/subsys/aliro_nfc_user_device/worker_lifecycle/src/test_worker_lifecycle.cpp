/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include <zephyr/kernel.h>
#include <zephyr/ztest.h>

#include "fake_nfc_interface.h"
#include "platform/nfc/nfc_worker.h"

#include <vector>

using namespace AliroUd::Nfc;

namespace {

/*
 * Generous relative to the worker thread's non-blocking dispatch: lets the
 * dedicated NFC/stack worker thread (started once for the whole suite)
 * drain every already-queued event before assertions run.
 */
void SettleWorker()
{
	k_msleep(50);
}

void *SetupWorker(void)
{
	StartWorker();
	SettleWorker();
	return nullptr;
}

void ResetBeforeEachTest(void *fixture)
{
	(void)fixture;
	AliroUdTest::FakeNfc::Reset();
}

/** @brief The Expedited Phase AID (Aliro 1.0 Specification and Test Plan, 26-42802-001, Table 10-3, page 96). */
const std::vector<uint8_t> kExpeditedPhaseAid{ 0xA0, 0x00, 0x00, 0x09, 0x09, 0xAC, 0xCE, 0x55, 0x01 };

std::vector<uint8_t> SelectCommand(const std::vector<uint8_t> &aid, uint8_t p1 = 0x04, uint8_t p2 = 0x00)
{
	std::vector<uint8_t> cmd{ 0x00, 0xA4, p1, p2, static_cast<uint8_t>(aid.size()) };
	cmd.insert(cmd.end(), aid.begin(), aid.end());
	return cmd;
}

/**
 * @brief Fixed FCI response for the Expedited Phase SELECT (Aliro 1.0
 * Specification and Test Plan, 26-42802-001, section 10.2.1.2, Appendix
 * 14.4, page 177).
 */
const std::vector<uint8_t> kExpectedFciResponse{
	0x6F, 0x15, 0x84, 0x09, 0xA0, 0x00, 0x00, 0x09, 0x09, 0xAC, 0xCE, 0x55, 0x01,
	0xA5, 0x08, 0x80, 0x02, 0x00, 0x00, 0x5C, 0x02, 0x01, 0x00, 0x90, 0x00,
};

} // namespace

ZTEST_SUITE(aliro_ud_worker_lifecycle, nullptr, SetupWorker, ResetBeforeEachTest, nullptr, nullptr);

/**
 * @brief A field-on event, followed by a command APDU, is dispatched
 * through the real worker thread and real UserDeviceStack: activation and
 * command handling produce exactly one response (NFC-to-stack call
 * ordering, response-buffer round trip).
 */
ZTEST(aliro_ud_worker_lifecycle, test_field_on_then_command_produces_one_response)
{
	zassert_equal(0, PostFieldOn(), "PostFieldOn() should succeed");
	SettleWorker();

	const auto command = SelectCommand(kExpeditedPhaseAid);
	zassert_equal(0, PostCommandApdu(command.data(), command.size()), "PostCommandApdu() should succeed");
	SettleWorker();

	zassert_equal(1u, AliroUdTest::FakeNfc::GetSentResponseCount(), "Expected exactly one response");

	zassert_equal(0, PostFieldOff(), "PostFieldOff() should succeed");
	SettleWorker();
}

/**
 * @brief A well-formed SELECT for the Expedited Phase AID, sent through the
 * real worker thread and stack, returns the exact FCI response bytes from
 * the spec's worked example, terminated with SW `9000`.
 */
ZTEST(aliro_ud_worker_lifecycle, test_select_expedited_aid_returns_spec_fci)
{
	zassert_equal(0, PostFieldOn());
	SettleWorker();

	const auto command = SelectCommand(kExpeditedPhaseAid);
	zassert_equal(0, PostCommandApdu(command.data(), command.size()));
	SettleWorker();

	const auto response = AliroUdTest::FakeNfc::GetLastResponse();
	zassert_equal(kExpectedFciResponse.size(), response.size(), "Unexpected FCI response length");
	zassert_mem_equal(kExpectedFciResponse.data(), response.data(), response.size(),
			   "FCI response does not match the Aliro spec worked example");

	zassert_equal(0, PostFieldOff());
	SettleWorker();
}

/**
 * @brief Field loss notifies `Nfc::HandleTermination()` (via
 * `UserDeviceSessionManager::Destroy()`), exercising the worker's
 * field-off -> DeactivateSession -> HandleTermination path end to end.
 */
ZTEST(aliro_ud_worker_lifecycle, test_field_off_notifies_termination)
{
	zassert_equal(0, PostFieldOn());
	SettleWorker();

	zassert_equal(0, PostFieldOff());
	SettleWorker();

	zassert_equal(1u, AliroUdTest::FakeNfc::GetTerminationCount(), "Expected exactly one termination notification");
}

/**
 * @brief A duplicate field-on (two field-on events with no intervening
 * field-off) is absorbed safely by the stack's session manager; the worker
 * keeps dispatching commands normally afterward.
 */
ZTEST(aliro_ud_worker_lifecycle, test_duplicate_field_on_is_safe)
{
	zassert_equal(0, PostFieldOn());
	zassert_equal(0, PostFieldOn());
	SettleWorker();

	const auto command = SelectCommand(kExpeditedPhaseAid);
	zassert_equal(0, PostCommandApdu(command.data(), command.size()));
	SettleWorker();

	zassert_equal(1u, AliroUdTest::FakeNfc::GetSentResponseCount(),
		      "Duplicate activation should not break normal command dispatch");

	zassert_equal(0, PostFieldOff());
	SettleWorker();
}

/**
 * @brief A stale/duplicate field-off with no active session (already torn
 * down, or a field-off that never had a matching field-on) is a safe no-op:
 * no crash, and no spurious termination notification.
 */
ZTEST(aliro_ud_worker_lifecycle, test_field_off_without_session_is_safe)
{
	zassert_equal(0, PostFieldOff());
	SettleWorker();

	zassert_equal(0u, AliroUdTest::FakeNfc::GetTerminationCount(),
		      "No session existed, so no termination should have been notified");
}

/**
 * @brief A field-loss/re-activation race (field-off immediately followed by
 * field-on, as the worker would see if a reader briefly drops and regains
 * the field) tears down the old session and cleanly activates a new one.
 */
ZTEST(aliro_ud_worker_lifecycle, test_field_loss_then_reactivate)
{
	zassert_equal(0, PostFieldOn());
	SettleWorker();

	/* Field-loss race: off immediately followed by on, before settling. */
	zassert_equal(0, PostFieldOff());
	zassert_equal(0, PostFieldOn());
	SettleWorker();

	const auto command = SelectCommand(kExpeditedPhaseAid);
	zassert_equal(0, PostCommandApdu(command.data(), command.size()));
	SettleWorker();

	zassert_equal(1u, AliroUdTest::FakeNfc::GetSentResponseCount(), "Reactivated session should handle commands");

	zassert_equal(0, PostFieldOff());
	SettleWorker();
}

/**
 * @brief Posting more events than the bounded queue depth
 * (`CONFIG_ALIRO_UD_NFC_EVENT_QUEUE_DEPTH`) without letting the worker drain
 * is rejected deterministically (never blocks, never corrupts state); once
 * the worker catches up, normal dispatch resumes.
 */
ZTEST(aliro_ud_worker_lifecycle, test_queue_overflow_is_deterministic_and_recoverable)
{
	size_t rejected{ 0 };

	/*
	 * CONFIG_ALIRO_UD_NFC_EVENT_QUEUE_DEPTH=4 (prj.conf); posting more
	 * than that back-to-back, without yielding, guarantees overflow: this
	 * test thread runs at a higher priority than the worker thread
	 * (K_PRIO_PREEMPT(CONFIG_ALIRO_UD_NFC_THREAD_PRIORITY), default 5), so
	 * the worker cannot drain anything until this loop yields/sleeps.
	 */
	for (int i = 0; i < 8; i++) {
		if (PostFieldOn() != 0) {
			rejected++;
		}
	}

	zassert_true(rejected > 0, "Expected at least one event to be rejected by the bounded queue");

	/* Let the worker fully drain the (duplicate) field-on events it did accept. */
	SettleWorker();

	/* Recovery: normal dispatch resumes cleanly after the overflow. */
	const auto command = SelectCommand(kExpeditedPhaseAid);
	zassert_equal(0, PostCommandApdu(command.data(), command.size()));
	SettleWorker();

	zassert_equal(1u, AliroUdTest::FakeNfc::GetSentResponseCount(), "Worker did not recover after queue overflow");

	zassert_equal(0, PostFieldOff());
	SettleWorker();
}
