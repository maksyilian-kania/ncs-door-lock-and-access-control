/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#pragma once

#include <cstddef>

/**
 * @brief Power-loss model shared by the suite's persistence and PSA fakes.
 *
 * Every durable mutation (record save or erase, durable key creation or
 * destruction) asks `AllowDurableWrite()` first. After `CutAfter(n)`, the
 * first `n` mutations land and every later one fails without effect, as if
 * power were lost at that point. Reads are unaffected. A test then
 * simulates the reboot with `Reset()` and `FakePsa::Reboot()`.
 */
namespace AliroUd::PersistentKey::FakePower {

/** @brief Restores power: every mutation lands and the trip flag clears. */
void Reset();

/** @brief Lets exactly `durableWrites` more mutations land. */
void CutAfter(size_t durableWrites);

/** @brief Returns true once a mutation was refused since the last `Reset()`. */
bool Tripped();

/** @brief Returns true if the next durable mutation may land; consumes one allowance. */
bool AllowDurableWrite();

} // namespace AliroUd::PersistentKey::FakePower
