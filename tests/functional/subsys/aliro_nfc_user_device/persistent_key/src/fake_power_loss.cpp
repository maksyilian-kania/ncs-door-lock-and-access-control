/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include "fake_power_loss.h"

#include <array>

namespace AliroUd::PersistentKey::FakePower {
namespace {

enum class Armed : uint8_t {
	None,
	Cut,
	FailOnce,
};

Armed sArmed{ Armed::None };
size_t sRemaining{ 0 };
bool sWithEffect{ false };
bool sTripped{ false };

std::array<Transition, 64> sTrace{};
size_t sTraceLength{ 0 };

} // namespace

void Reset()
{
	sArmed = Armed::None;
	sRemaining = 0;
	sWithEffect = false;
	sTripped = false;
	ClearTrace();
}

void CutAfter(size_t durableWrites)
{
	sArmed = Armed::Cut;
	sRemaining = durableWrites;
	sTripped = false;
}

void FailOnceAfter(size_t durableWrites, bool withEffect)
{
	sArmed = Armed::FailOnce;
	sRemaining = durableWrites;
	sWithEffect = withEffect;
	sTripped = false;
}

bool Tripped()
{
	return sTripped;
}

Outcome NextDurableWrite(Mutation kind, uint32_t target)
{
	if (sTraceLength < sTrace.size()) {
		sTrace[sTraceLength] = Transition{ kind, target };
	}
	++sTraceLength;

	if (sArmed == Armed::None) {
		return Outcome::Land;
	}
	if (sRemaining > 0) {
		--sRemaining;
		return Outcome::Land;
	}

	sTripped = true;
	if (sArmed == Armed::Cut) {
		return Outcome::Refuse;
	}
	sArmed = Armed::None;
	return sWithEffect ? Outcome::LandThenFail : Outcome::Refuse;
}

void ClearTrace()
{
	sTraceLength = 0;
}

size_t TraceLength()
{
	return sTraceLength;
}

Transition TraceAt(size_t index)
{
	return index < sTrace.size() ? sTrace[index] : Transition{};
}

size_t TraceCapacity()
{
	return sTrace.size();
}

} // namespace AliroUd::PersistentKey::FakePower
