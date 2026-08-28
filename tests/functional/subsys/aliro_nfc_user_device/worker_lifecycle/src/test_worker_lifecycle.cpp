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
 * drain every already-queued event/pending field transition before
 * assertions run.
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

	/*
	 * Tests that end without an explicit PostFieldOff() (e.g. the timeout
	 * test) must not leak an active session into the next test case.
	 */
	if (IsSessionActive()) {
		PostFieldOff();
		SettleWorker();
	}
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
	PostFieldOn();
	SettleWorker();
	zassert_true(IsSessionActive(), "Session should be active after FIELD_ON");

	const auto command = SelectCommand(kExpeditedPhaseAid);
	zassert_equal(0, PostCommandApdu(command.data(), command.size()), "PostCommandApdu() should succeed");
	SettleWorker();

	zassert_equal(1u, AliroUdTest::FakeNfc::GetSentResponseCount(), "Expected exactly one response");

	PostFieldOff();
	SettleWorker();
	zassert_false(IsSessionActive(), "Session should be inactive after FIELD_OFF");
}

/**
 * @brief A well-formed SELECT for the Expedited Phase AID, sent through the
 * real worker thread and stack, returns the exact FCI response bytes from
 * the spec's worked example, terminated with SW `9000`.
 */
ZTEST(aliro_ud_worker_lifecycle, test_select_expedited_aid_returns_spec_fci)
{
	PostFieldOn();
	SettleWorker();

	const auto command = SelectCommand(kExpeditedPhaseAid);
	zassert_equal(0, PostCommandApdu(command.data(), command.size()));
	SettleWorker();

	const auto response = AliroUdTest::FakeNfc::GetLastResponse();
	zassert_equal(kExpectedFciResponse.size(), response.size(), "Unexpected FCI response length");
	zassert_mem_equal(kExpectedFciResponse.data(), response.data(), response.size(),
			   "FCI response does not match the Aliro spec worked example");

	PostFieldOff();
	SettleWorker();
}

/**
 * @brief Field loss notifies `Nfc::HandleTermination()` (via
 * `UserDeviceSessionManager::Destroy()`), exercising the worker's
 * field-off -> DeactivateSession -> HandleTermination path end to end, and
 * the resulting local "session active" flag is cleared.
 */
ZTEST(aliro_ud_worker_lifecycle, test_field_off_notifies_termination)
{
	PostFieldOn();
	SettleWorker();

	PostFieldOff();
	SettleWorker();

	zassert_equal(1u, AliroUdTest::FakeNfc::GetTerminationCount(), "Expected exactly one termination notification");
	zassert_false(IsSessionActive(), "Session should be inactive after termination");
}

/**
 * @brief A duplicate field-on (two field-on events with no intervening
 * field-off) results in exactly one activation attempt: the worker's local
 * "session active" flag makes the second FIELD_ON a no-op rather than a
 * second `ActivateSession()` call.
 */
ZTEST(aliro_ud_worker_lifecycle, test_duplicate_field_on_is_safe)
{
	const auto attemptsBefore = GetActivationAttemptCount();

	PostFieldOn();
	SettleWorker();
	PostFieldOn();
	SettleWorker();

	zassert_equal(attemptsBefore + 1, GetActivationAttemptCount(),
		      "Duplicate FIELD_ON must not trigger a second activation attempt");

	const auto command = SelectCommand(kExpeditedPhaseAid);
	zassert_equal(0, PostCommandApdu(command.data(), command.size()));
	SettleWorker();

	zassert_equal(1u, AliroUdTest::FakeNfc::GetSentResponseCount(),
		      "Duplicate activation should not break normal command dispatch");

	PostFieldOff();
	SettleWorker();
}

/**
 * @brief A stale/duplicate field-off with no active session (already torn
 * down, or a field-off that never had a matching field-on) is a safe no-op:
 * no crash, no spurious termination notification, and no
 * `DeactivateSession()` call.
 */
ZTEST(aliro_ud_worker_lifecycle, test_field_off_without_session_is_safe)
{
	PostFieldOff();
	SettleWorker();

	zassert_equal(0u, AliroUdTest::FakeNfc::GetTerminationCount(),
		      "No session existed, so no termination should have been notified");
	zassert_false(IsSessionActive());
}

/**
 * @brief ON -> APDUs -> ON -> APDUs -> OFF: normal traffic interleaved with
 * a duplicate, ignored FIELD_ON produces exactly one activation attempt and
 * every command APDU is still answered.
 */
ZTEST(aliro_ud_worker_lifecycle, test_on_apdus_on_apdus_off)
{
	const auto attemptsBefore = GetActivationAttemptCount();
	const auto command = SelectCommand(kExpeditedPhaseAid);

	PostFieldOn();
	SettleWorker();

	zassert_equal(0, PostCommandApdu(command.data(), command.size()));
	SettleWorker();
	zassert_equal(1u, AliroUdTest::FakeNfc::GetSentResponseCount());

	/* Duplicate FIELD_ON: ignored, no second activation attempt. */
	PostFieldOn();
	SettleWorker();
	zassert_equal(attemptsBefore + 1, GetActivationAttemptCount());

	zassert_equal(0, PostCommandApdu(command.data(), command.size()));
	SettleWorker();
	zassert_equal(2u, AliroUdTest::FakeNfc::GetSentResponseCount());

	PostFieldOff();
	SettleWorker();
	zassert_false(IsSessionActive());
}

/**
 * @brief ON -> OFF -> ON: strict, non-racing field cycle. Deactivation
 * completes fully before the second activation, which must succeed
 * normally (fresh session, one more activation attempt).
 */
ZTEST(aliro_ud_worker_lifecycle, test_on_off_on)
{
	const auto attemptsBefore = GetActivationAttemptCount();

	PostFieldOn();
	SettleWorker();
	zassert_true(IsSessionActive());

	PostFieldOff();
	SettleWorker();
	zassert_false(IsSessionActive());

	PostFieldOn();
	SettleWorker();
	zassert_true(IsSessionActive());
	zassert_equal(attemptsBefore + 2, GetActivationAttemptCount());

	const auto command = SelectCommand(kExpeditedPhaseAid);
	zassert_equal(0, PostCommandApdu(command.data(), command.size()));
	SettleWorker();
	zassert_equal(1u, AliroUdTest::FakeNfc::GetSentResponseCount(), "Reactivated session should handle commands");

	PostFieldOff();
	SettleWorker();
}

/**
 * @brief A field-loss/re-activation race (field-off immediately followed by
 * field-on, as the worker would see if a reader briefly drops and regains
 * the field) tears down the old session and cleanly activates a new one.
 * FIELD_ON/FIELD_OFF are coalesced onto one pending-intent slot, so only
 * the latest (FIELD_ON) survives here - which is the correct outcome.
 */
ZTEST(aliro_ud_worker_lifecycle, test_field_loss_then_reactivate)
{
	PostFieldOn();
	SettleWorker();

	/* Field-loss race: off immediately followed by on, before settling. */
	PostFieldOff();
	PostFieldOn();
	SettleWorker();

	zassert_true(IsSessionActive(), "Latest coalesced intent (FIELD_ON) should win the race");

	const auto command = SelectCommand(kExpeditedPhaseAid);
	zassert_equal(0, PostCommandApdu(command.data(), command.size()));
	SettleWorker();

	zassert_equal(1u, AliroUdTest::FakeNfc::GetSentResponseCount(), "Reactivated session should handle commands");

	PostFieldOff();
	SettleWorker();
}

/**
 * @brief ON -> timeout termination -> ON: the stack's own session watchdog
 * (`CONFIG_NCS_ALIRO_USER_DEVICE_SESSION_TIMEOUT_NFC`, shortened for this
 * suite) terminates the session independently of any FIELD_OFF. The
 * worker's local "session active" flag must follow that termination
 * (via `NotifySessionTerminated()`), and a subsequent FIELD_ON must
 * activate a fresh session rather than being ignored as "already active".
 */
ZTEST(aliro_ud_worker_lifecycle, test_on_timeout_termination_on)
{
	const auto attemptsBefore = GetActivationAttemptCount();
	const auto terminationsBefore = AliroUdTest::FakeNfc::GetTerminationCount();

	PostFieldOn();
	SettleWorker();
	zassert_true(IsSessionActive());

	/* CONFIG_NCS_ALIRO_USER_DEVICE_SESSION_TIMEOUT_NFC=500 (prj.conf). */
	k_msleep(800);
	SettleWorker();

	zassert_equal(terminationsBefore + 1, AliroUdTest::FakeNfc::GetTerminationCount(),
		      "Watchdog timeout should have terminated the session");
	zassert_false(IsSessionActive(), "Local session-active belief must follow stack-driven termination");

	PostFieldOn();
	SettleWorker();
	zassert_true(IsSessionActive(), "FIELD_ON after a stack-driven termination must reactivate");
	zassert_equal(attemptsBefore + 2, GetActivationAttemptCount());

	PostFieldOff();
	SettleWorker();
}

/**
 * @brief Posting more command APDUs than the bounded queue depth
 * (`CONFIG_ALIRO_UD_NFC_EVENT_QUEUE_DEPTH`) without letting the worker
 * drain forces deterministic session recovery; dispatch resumes cleanly
 * afterward once a fresh FIELD_ON reactivates.
 */
ZTEST(aliro_ud_worker_lifecycle, test_queue_overflow_is_deterministic_and_recoverable)
{
	PostFieldOn();
	SettleWorker();
	zassert_true(IsSessionActive());

	size_t rejected{ 0 };
	const auto command = SelectCommand(kExpeditedPhaseAid);

	/*
	 * CONFIG_ALIRO_UD_NFC_EVENT_QUEUE_DEPTH=4 (prj.conf); posting more
	 * than that back-to-back, without yielding, guarantees overflow: this
	 * test thread runs at a higher priority than the worker thread
	 * (K_PRIO_PREEMPT(CONFIG_ALIRO_UD_NFC_THREAD_PRIORITY), default 5), so
	 * the worker cannot drain anything until this loop yields/sleeps.
	 */
	for (int i = 0; i < 8; i++) {
		if (PostCommandApdu(command.data(), command.size()) != 0) {
			rejected++;
		}
	}

	zassert_true(rejected > 0, "Expected at least one event to be rejected by the bounded queue");

	/* The overflow must force deterministic recovery: session torn down. */
	SettleWorker();
	zassert_false(IsSessionActive(), "Overflow must force session recovery rather than leaving stale state");

	/* Recovery: a fresh field cycle resumes normal dispatch cleanly. */
	PostFieldOn();
	SettleWorker();
	zassert_equal(0, PostCommandApdu(command.data(), command.size()));
	SettleWorker();

	zassert_equal(1u, AliroUdTest::FakeNfc::GetSentResponseCount(), "Worker did not recover after queue overflow");

	PostFieldOff();
	SettleWorker();
}

/**
 * @brief ON -> queue overflow involving OFF: flooding the bounded queue
 * with command APDUs must not cause a subsequent FIELD_OFF to be dropped.
 * FIELD_OFF is delivered through the separate coalesced field channel, so
 * it is never subject to the bounded queue's capacity.
 */
ZTEST(aliro_ud_worker_lifecycle, test_queue_overflow_does_not_drop_field_off)
{
	const auto terminationsBefore = AliroUdTest::FakeNfc::GetTerminationCount();

	PostFieldOn();
	SettleWorker();
	zassert_true(IsSessionActive());

	const auto command = SelectCommand(kExpeditedPhaseAid);
	for (int i = 0; i < 8; i++) {
		PostCommandApdu(command.data(), command.size());
	}

	/* Posted while the command queue is still full/being drained. */
	PostFieldOff();
	SettleWorker();

	zassert_false(IsSessionActive(), "FIELD_OFF must still take effect despite queue overflow");
	zassert_true(AliroUdTest::FakeNfc::GetTerminationCount() >= terminationsBefore + 1,
		     "FIELD_OFF (or the overflow's forced recovery) must have terminated the session");
}

/**
 * @brief A command APDU received with no active session - either before
 * any FIELD_ON, or after a session has already terminated - is rejected
 * deterministically: no `HandleCommandApdu()` call reaches the stack (no
 * response is sent) and the drop is counted.
 */
ZTEST(aliro_ud_worker_lifecycle, test_apdu_rejected_without_active_session)
{
	const auto rejectedBefore = GetRejectedApduCount();
	const auto command = SelectCommand(kExpeditedPhaseAid);

	/* Before any FIELD_ON. */
	zassert_equal(0, PostCommandApdu(command.data(), command.size()));
	SettleWorker();
	zassert_equal(0u, AliroUdTest::FakeNfc::GetSentResponseCount(), "No session: no response should be sent");
	zassert_equal(rejectedBefore + 1, GetRejectedApduCount());

	/* After a session existed and was terminated. */
	PostFieldOn();
	SettleWorker();
	PostFieldOff();
	SettleWorker();
	zassert_false(IsSessionActive());

	zassert_equal(0, PostCommandApdu(command.data(), command.size()));
	SettleWorker();
	zassert_equal(0u, AliroUdTest::FakeNfc::GetSentResponseCount(),
		      "Post-termination APDU must still be rejected, not answered");
	zassert_equal(rejectedBefore + 2, GetRejectedApduCount());
}
