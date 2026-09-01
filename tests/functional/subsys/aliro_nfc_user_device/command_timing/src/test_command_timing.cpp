/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include <zephyr/kernel.h>
#include <zephyr/ztest.h>

#include "fake_nfc_interface.h"
#include "platform/nfc/command_timing.h"
#include "platform/nfc/nfc_worker.h"

#include <vector>

using namespace AliroUd::Nfc;

namespace {

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
	ResetCommandTimingStats();

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

} // namespace

#if defined(CONFIG_ALIRO_UD_TIMING_INSTRUMENTATION)

/*
 * Pure logic tests for the `CommandTiming` class (APP_PLAN.md AWP7 Verify:
 * "monotonic duration recording, wraparound handling"). These drive the
 * class directly with an explicit fake `nowMs` and never touch the worker
 * thread or a real clock, so they are deterministic and instantaneous.
 */
ZTEST_SUITE(aliro_ud_command_timing_logic, nullptr, nullptr, nullptr, nullptr, nullptr);

ZTEST(aliro_ud_command_timing_logic, test_begin_end_records_one_sample)
{
	CommandTiming timing{};

	zassert_false(timing.HasPending());
	timing.Begin(1000);
	zassert_true(timing.HasPending());

	timing.End(1015);
	zassert_false(timing.HasPending());
	zassert_equal(1u, timing.GetSampleCount());
	zassert_equal(15u, timing.GetLastDurationMs());
	zassert_equal(15u, timing.GetMaxDurationMs());
}

ZTEST(aliro_ud_command_timing_logic, test_end_without_begin_is_noop)
{
	CommandTiming timing{};

	timing.End(1234);

	zassert_equal(0u, timing.GetSampleCount(), "End() with no pending Begin() must not record a sample");
	zassert_equal(0u, timing.GetLastDurationMs());
	zassert_equal(0u, timing.GetMaxDurationMs());
}

ZTEST(aliro_ud_command_timing_logic, test_max_tracks_largest_sample)
{
	CommandTiming timing{};

	timing.Begin(0);
	timing.End(10);
	zassert_equal(10u, timing.GetMaxDurationMs());

	timing.Begin(100);
	timing.End(103);
	zassert_equal(3u, timing.GetLastDurationMs());
	zassert_equal(10u, timing.GetMaxDurationMs(), "Max must not shrink after a smaller sample");

	timing.Begin(200);
	timing.End(250);
	zassert_equal(50u, timing.GetLastDurationMs());
	zassert_equal(50u, timing.GetMaxDurationMs(), "Max must grow when a larger sample is recorded");

	zassert_equal(3u, timing.GetSampleCount());
}

ZTEST(aliro_ud_command_timing_logic, test_reset_stats_clears_counters_but_not_pending)
{
	CommandTiming timing{};

	timing.Begin(0);
	timing.End(20);
	zassert_equal(1u, timing.GetSampleCount());

	timing.Begin(50);
	timing.ResetStats();

	zassert_equal(0u, timing.GetSampleCount());
	zassert_equal(0u, timing.GetLastDurationMs());
	zassert_equal(0u, timing.GetMaxDurationMs());
	zassert_true(timing.HasPending(), "ResetStats() must not cancel a pending Begin()");

	timing.End(80);
	zassert_equal(1u, timing.GetSampleCount());
	zassert_equal(30u, timing.GetLastDurationMs());
}

ZTEST(aliro_ud_command_timing_logic, test_wraparound_duration_is_correct)
{
	CommandTiming timing{};

	/* k_uptime_get_32() wraps at 2^32; unsigned subtraction must still yield the true elapsed time. */
	const uint32_t nearWrap = 0xFFFFFFF0U;
	timing.Begin(nearWrap);
	timing.End(5); /* Wrapped past 0, five ms after. */

	const uint32_t expected = static_cast<uint32_t>(5 - nearWrap); /* Modulo-2^32 arithmetic: 0x15. */
	zassert_equal(expected, timing.GetLastDurationMs());
	zassert_equal(0x15u, timing.GetLastDurationMs());
	zassert_equal(1u, timing.GetSampleCount());
}

ZTEST(aliro_ud_command_timing_logic, test_duplicate_begin_overwrites_start)
{
	CommandTiming timing{};

	timing.Begin(0);
	timing.Begin(100); /* A stray second Begin() before any End() overwrites the pending start. */
	timing.End(110);

	zassert_equal(10u, timing.GetLastDurationMs(), "Latest Begin() must win, not the first");
}

#endif /* CONFIG_ALIRO_UD_TIMING_INSTRUMENTATION */

/*
 * Integration tests through the real worker thread + real
 * Aliro::UserDeviceStack (fake NFC transport, same pattern as
 * worker_lifecycle): confirms BeginCommandTiming()/EndCommandTiming() are
 * actually wired into the command-APDU path, and that the disabled build
 * behaves as a true no-op (APP_PLAN.md AWP7).
 */
ZTEST_SUITE(aliro_ud_command_timing_integration, nullptr, SetupWorker, ResetBeforeEachTest, nullptr, nullptr);

ZTEST(aliro_ud_command_timing_integration, test_snapshot_reports_build_time_enabled_flag)
{
	const auto snapshot = GetCommandTimingSnapshot();

#if defined(CONFIG_ALIRO_UD_TIMING_INSTRUMENTATION)
	zassert_true(snapshot.mEnabled);
#else
	zassert_false(snapshot.mEnabled);
#endif
}

ZTEST(aliro_ud_command_timing_integration, test_command_apdu_updates_or_stays_disabled)
{
	PostFieldOn();
	SettleWorker();

	const auto command = SelectCommand(kExpeditedPhaseAid);
	zassert_equal(0, PostCommandApdu(command.data(), command.size()));
	SettleWorker();

	zassert_equal(1u, AliroUdTest::FakeNfc::GetSentResponseCount(), "Sanity: the command was actually answered");

	const auto snapshot = GetCommandTimingSnapshot();

#if defined(CONFIG_ALIRO_UD_TIMING_INSTRUMENTATION)
	zassert_true(snapshot.mEnabled);
	zassert_equal(1u, snapshot.mSampleCount,
		      "One command APDU handled end to end should record exactly one timing sample");
#else
	zassert_false(snapshot.mEnabled);
	zassert_equal(0u, snapshot.mSampleCount,
		      "Disabled build must record no timing samples no matter how much NFC traffic runs");
	zassert_equal(0u, snapshot.mLastDurationMs);
	zassert_equal(0u, snapshot.mMaxDurationMs);
#endif

	PostFieldOff();
	SettleWorker();
}

ZTEST(aliro_ud_command_timing_integration, test_multiple_commands_accumulate_samples)
{
	PostFieldOn();
	SettleWorker();

	const auto command = SelectCommand(kExpeditedPhaseAid);
	for (int i = 0; i < 3; ++i) {
		zassert_equal(0, PostCommandApdu(command.data(), command.size()));
		SettleWorker();
	}

	zassert_equal(3u, AliroUdTest::FakeNfc::GetSentResponseCount());

	const auto snapshot = GetCommandTimingSnapshot();

#if defined(CONFIG_ALIRO_UD_TIMING_INSTRUMENTATION)
	zassert_equal(3u, snapshot.mSampleCount, "Three handled commands should record three timing samples");
	zassert_true(snapshot.mMaxDurationMs >= snapshot.mLastDurationMs || snapshot.mMaxDurationMs == 0,
		     "Max duration must never be smaller than a valid recorded sample");
#else
	zassert_equal(0u, snapshot.mSampleCount);
#endif

	PostFieldOff();
	SettleWorker();
}

ZTEST(aliro_ud_command_timing_integration, test_reset_clears_snapshot)
{
	PostFieldOn();
	SettleWorker();

	const auto command = SelectCommand(kExpeditedPhaseAid);
	zassert_equal(0, PostCommandApdu(command.data(), command.size()));
	SettleWorker();

	ResetCommandTimingStats();

	const auto snapshot = GetCommandTimingSnapshot();
	zassert_equal(0u, snapshot.mSampleCount, "ResetCommandTimingStats() must clear the sample count");
	zassert_equal(0u, snapshot.mLastDurationMs);
	zassert_equal(0u, snapshot.mMaxDurationMs);

	PostFieldOff();
	SettleWorker();
}

ZTEST(aliro_ud_command_timing_integration, test_rejected_apdu_without_session_does_not_record_a_sample)
{
	/* No active session: HandleCommandApdu() is never reached, so BeginCommandTiming()/EndCommandTiming() must not run either. */
	const auto command = SelectCommand(kExpeditedPhaseAid);
	zassert_equal(0, PostCommandApdu(command.data(), command.size()));
	SettleWorker();

	zassert_equal(0u, AliroUdTest::FakeNfc::GetSentResponseCount());

	const auto snapshot = GetCommandTimingSnapshot();
	zassert_equal(0u, snapshot.mSampleCount, "A rejected APDU (no session) must not be counted as a timed command");
}
