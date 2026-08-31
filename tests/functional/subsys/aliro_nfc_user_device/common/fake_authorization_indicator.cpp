/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include "fake_authorization_indicator.h"

#include <platform/authorization/authorization_indicator.h>

namespace AliroUd::Authorization::Test {
namespace {

size_t sSetActiveCallCount{ 0 };
bool sLastActive{ false };

} // namespace

void ResetFakeAuthorizationIndicator()
{
	sSetActiveCallCount = 0;
	sLastActive = false;
}

size_t GetSetActiveCallCount()
{
	return sSetActiveCallCount;
}

bool GetLastActive()
{
	return sLastActive;
}

} // namespace AliroUd::Authorization::Test

namespace AliroUd::Authorization::Indicator {

void SetActive(bool active)
{
	++AliroUd::Authorization::Test::sSetActiveCallCount;
	AliroUd::Authorization::Test::sLastActive = active;
}

} // namespace AliroUd::Authorization::Indicator
