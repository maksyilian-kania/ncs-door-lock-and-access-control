/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include "platform/nfc/command_timing.h"

#include <zephyr/kernel.h>

namespace AliroUd::Nfc {

void CommandTiming::Begin(uint32_t nowMs)
{
	mPending = true;
	mStartMs = nowMs;
}

void CommandTiming::End(uint32_t nowMs)
{
	if (!mPending) {
		return;
	}

	mPending = false;

	/* Unsigned subtraction: correct modulo 2^32 even if nowMs wrapped past mStartMs. */
	const uint32_t duration = nowMs - mStartMs;
	mLastDurationMs = duration;
	mMaxDurationMs = duration > mMaxDurationMs ? duration : mMaxDurationMs;
	++mSampleCount;
}

void CommandTiming::ResetStats()
{
	mSampleCount = 0;
	mLastDurationMs = 0;
	mMaxDurationMs = 0;
}

namespace {

/*
 * Worker-thread-only, like every other command-processing variable in
 * nfc_worker.cpp: BeginCommandTiming()/EndCommandTiming() are only ever
 * called from there, immediately before/after the one HandleCommandApdu()
 * call. GetCommandTimingSnapshot()/ResetCommandTimingStats() are read/reset
 * from the CLI shell thread (diagnostics only, not part of the timing
 * measurement itself); a torn read of these plain integers is at worst a
 * stale/inconsistent diagnostic snapshot, never a correctness issue for the
 * worker thread's own bookkeeping.
 */
CommandTiming sTiming{};

} // namespace

void BeginCommandTiming()
{
	sTiming.Begin(k_uptime_get_32());
}

void EndCommandTiming()
{
	sTiming.End(k_uptime_get_32());
}

CommandTimingSnapshot GetCommandTimingSnapshot()
{
	return CommandTimingSnapshot{ true, sTiming.GetSampleCount(), sTiming.GetLastDurationMs(),
				      sTiming.GetMaxDurationMs() };
}

void ResetCommandTimingStats()
{
	sTiming.ResetStats();
}

} // namespace AliroUd::Nfc
