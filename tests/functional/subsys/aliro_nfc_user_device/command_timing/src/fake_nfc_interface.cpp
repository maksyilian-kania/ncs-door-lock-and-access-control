/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include "fake_nfc_interface.h"

#include "platform/nfc/nfc_worker.h"

#include <aliro/user_device/interface.h>

namespace {

size_t gSentResponseCount{ 0 };
std::vector<uint8_t> gLastResponse{};
size_t gTerminationCount{ 0 };

} // namespace

namespace AliroUdTest::FakeNfc {

void Reset()
{
	gSentResponseCount = 0;
	gLastResponse.clear();
	gTerminationCount = 0;
}

size_t GetSentResponseCount()
{
	return gSentResponseCount;
}

std::vector<uint8_t> GetLastResponse()
{
	return gLastResponse;
}

size_t GetTerminationCount()
{
	return gTerminationCount;
}

} // namespace AliroUdTest::FakeNfc

namespace Aliro::Interface::UserDevice::Nfc {

AliroError SendResponseApdu(ConnectionHandle handle, ConstData apdu)
{
	(void)handle;

	gSentResponseCount++;
	gLastResponse.assign(apdu.mData, apdu.mData + apdu.mLength);

	return ALIRO_NO_ERROR;
}

void HandleTermination(ConnectionHandle handle)
{
	(void)handle;

	gTerminationCount++;

	/*
	 * Mirrors the real nfc_transport.cpp::HandleTermination(): the worker
	 * must learn that the stack ended the session so its local
	 * "session active" belief never goes stale (AWP1 lifecycle-race fix).
	 */
	AliroUd::Nfc::NotifySessionTerminated();
}

TimingConstraints GetTimingConstraints(ConnectionHandle handle)
{
	(void)handle;

	return TimingConstraints{};
}

} // namespace Aliro::Interface::UserDevice::Nfc
