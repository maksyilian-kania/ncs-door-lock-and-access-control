/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include <zephyr/ztest.h>

#include <aliro/user_device/user_device.h>

/*
 * AWP0 build/link evidence for ALIRO-UD-SYRS-P1-001: a host build that links
 * the checked-out Aliro::UserDeviceStack facade and calls its current public
 * Init() API. This is not protocol verification; session/APDU behavior is
 * exercised starting with AWP1.
 */
ZTEST_SUITE(aliro_nfc_user_device_host_smoke, nullptr, nullptr, nullptr, nullptr, nullptr);

ZTEST(aliro_nfc_user_device_host_smoke, test_stack_facade_links_and_initializes)
{
	const AliroError err = Aliro::UserDeviceStack::Instance().Init();

	zassert_equal(ALIRO_NO_ERROR, err.ToInt(), "UserDeviceStack::Init() unexpectedly failed");
}
