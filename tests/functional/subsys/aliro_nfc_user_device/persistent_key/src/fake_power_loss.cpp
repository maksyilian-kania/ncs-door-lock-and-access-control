/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include "fake_power_loss.h"

namespace AliroUd::PersistentKey::FakePower {
namespace {

bool sArmed{ false };
size_t sRemaining{ 0 };
bool sTripped{ false };

} // namespace

void Reset()
{
	sArmed = false;
	sRemaining = 0;
	sTripped = false;
}

void CutAfter(size_t durableWrites)
{
	sArmed = true;
	sRemaining = durableWrites;
	sTripped = false;
}

bool Tripped()
{
	return sTripped;
}

bool AllowDurableWrite()
{
	if (!sArmed) {
		return true;
	}
	if (sRemaining == 0) {
		sTripped = true;
		return false;
	}
	--sRemaining;
	return true;
}

} // namespace AliroUd::PersistentKey::FakePower
