/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#pragma once

#include <cstddef>
#include <cstdint>

/**
 * @brief Durable-mutation fault model shared by the suite's persistence and
 * PSA fakes.
 *
 * Every durable mutation (record save or erase, durable key creation or
 * destruction) asks `NextDurableWrite()` for its outcome, which is also
 * appended to a trace. After `CutAfter(n)`, the first `n` mutations land and
 * every later one fails without effect, as if power were lost at that
 * point. After `FailOnceAfter(n, withEffect)`, the first `n` mutations land
 * and the next one reports failure, without its effect or after it, while
 * later mutations land again. Reads are unaffected. A test then simulates
 * the reboot with `Reset()` and `FakePsa::Reboot()`.
 */
namespace AliroUd::PersistentKey::FakePower {

enum class Mutation : uint8_t {
	OwnKey,
	DestroyKey,
	SaveRecord,
	EraseRecord,
};

/** @brief One attempted durable mutation: a key ID for keys, a slot index for records. */
struct Transition {
	Mutation mKind{ Mutation::OwnKey };
	uint32_t mTarget{ 0 };

	bool operator==(const Transition &other) const { return mKind == other.mKind && mTarget == other.mTarget; }
};

enum class Outcome : uint8_t {
	Land,
	Refuse,
	LandThenFail,
};

/** @brief Restores power, disarms any fault, and clears the trip flag and the trace. */
void Reset();

/** @brief Lets exactly `durableWrites` more mutations land, then refuses every later one. */
void CutAfter(size_t durableWrites);

/**
 * @brief Lets exactly `durableWrites` more mutations land, then fails the
 * next one once: without its effect, or with it when `withEffect` is true.
 */
void FailOnceAfter(size_t durableWrites, bool withEffect);

/** @brief Returns true once a mutation was refused or failed since the last `Reset()`. */
bool Tripped();

/** @brief Decides the outcome of the next durable mutation and records it in the trace. */
Outcome NextDurableWrite(Mutation kind, uint32_t target);

/** @brief Clears the trace only. */
void ClearTrace();

/** @brief Number of mutations attempted since the trace was last cleared. */
size_t TraceLength();

/** @brief The `index`-th attempted mutation; valid for `index < TraceLength()` within the trace capacity. */
Transition TraceAt(size_t index);

/** @brief Maximum number of mutations the trace retains. */
size_t TraceCapacity();

} // namespace AliroUd::PersistentKey::FakePower
