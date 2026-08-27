/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include <aliro/user_device/interface.h>

namespace Aliro::Interface::UserDevice::Os {

std::optional<Time> GetTrustedTimestamp()
{
	/*
	 * No trusted wall-clock source (RTC/secure time sync) exists on this
	 * platform for P1 (APP_PLAN.md AWP1). Returning std::nullopt is a
	 * valid, spec-anticipated answer: callers must already treat a
	 * missing trusted time as "timestamp validity cannot be evaluated"
	 * rather than as an error.
	 */
	return std::nullopt;
}

} // namespace Aliro::Interface::UserDevice::Os
