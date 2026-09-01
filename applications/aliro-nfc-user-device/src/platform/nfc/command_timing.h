/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#pragma once

#include <cstddef>
#include <cstdint>

/*
 * Command-to-response duration instrumentation (APP_PLAN.md AWP7: "Measure
 * the application boundary from command delivery to response send.").
 *
 * `nfc_worker.cpp`'s call into `Aliro::UserDeviceStack::HandleCommandApdu()`
 * only returns after the stack has already called
 * `Aliro::Interface::UserDevice::Nfc::SendResponseApdu()` synchronously (see
 * `nfc_transport.cpp`), so wrapping that one call with `BeginCommandTiming()`
 * before and `EndCommandTiming()` after measures exactly the
 * delivery-to-response-sent boundary this application owns, without needing
 * any change to the response-transmission path itself.
 */
namespace AliroUd::Nfc {

/**
 * @brief Pure, host-testable command-to-response duration tracker.
 *
 * Single-writer only: intended for exclusive use from the dedicated NFC/
 * stack worker thread (see nfc_worker.cpp), matching every other
 * worker-thread-only variable in this module. Takes an explicit `nowMs`
 * rather than reading a clock itself so it can be unit-tested with a fake
 * monotonic clock (APP_PLAN.md AWP7 Verify: "monotonic duration recording,
 * wraparound handling").
 */
class CommandTiming {
public:
	/** @brief Records that a command APDU has just been delivered to the stack at `nowMs`. */
	void Begin(uint32_t nowMs);

	/**
	 * @brief Records that the corresponding response has just been sent at `nowMs`.
	 *
	 * A no-op if no `Begin()` is currently pending, so a stray or
	 * duplicate call can never corrupt statistics. Duration is computed
	 * with unsigned subtraction, which is correct modulo 2^32 even if the
	 * millisecond counter wrapped between `Begin()` and `End()`.
	 */
	void End(uint32_t nowMs);

	/** @brief Clears every recorded statistic. Does not cancel a currently pending `Begin()`. */
	void ResetStats();

	bool HasPending() const
	{
		return mPending;
	}

	size_t GetSampleCount() const
	{
		return mSampleCount;
	}

	uint32_t GetLastDurationMs() const
	{
		return mLastDurationMs;
	}

	uint32_t GetMaxDurationMs() const
	{
		return mMaxDurationMs;
	}

private:
	bool mPending{ false };
	uint32_t mStartMs{ 0 };
	size_t mSampleCount{ 0 };
	uint32_t mLastDurationMs{ 0 };
	uint32_t mMaxDurationMs{ 0 };
};

/**
 * @brief Snapshot of the process-wide command-timing statistics.
 *
 * `mEnabled` is false whenever `CONFIG_ALIRO_UD_TIMING_INSTRUMENTATION` is
 * disabled: every other field is then always zero regardless of how many
 * transactions actually ran (APP_PLAN.md AWP7: "removable from production
 * builds").
 */
struct CommandTimingSnapshot {
	bool mEnabled{ false };
	size_t mSampleCount{ 0 };
	uint32_t mLastDurationMs{ 0 };
	uint32_t mMaxDurationMs{ 0 };
};

#if defined(CONFIG_ALIRO_UD_TIMING_INSTRUMENTATION)

/** @brief Marks the start of one command-to-response measurement. Call right before handing a command APDU to the stack. */
void BeginCommandTiming();

/** @brief Marks the end of one command-to-response measurement. Call right after the stack returns from handling it. */
void EndCommandTiming();

/** @brief Returns the current process-wide command-timing statistics. */
CommandTimingSnapshot GetCommandTimingSnapshot();

/** @brief Resets the process-wide command-timing statistics (diagnostics/CLI use only). */
void ResetCommandTimingStats();

#else

/*
 * Removable from production builds (APP_PLAN.md AWP7): when the Kconfig
 * option is disabled these are trivial inline no-ops, so call sites in
 * nfc_worker.cpp/cli.cpp never need an #ifdef of their own.
 */
inline void BeginCommandTiming()
{
}

inline void EndCommandTiming()
{
}

inline CommandTimingSnapshot GetCommandTimingSnapshot()
{
	return CommandTimingSnapshot{};
}

inline void ResetCommandTimingStats()
{
}

#endif

} // namespace AliroUd::Nfc
