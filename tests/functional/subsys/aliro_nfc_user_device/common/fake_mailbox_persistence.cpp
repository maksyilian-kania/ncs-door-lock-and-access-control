/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include "fake_mailbox_persistence.h"

#include <storage/mailbox/mailbox_persistence.h>

#include <array>

using namespace AliroUd::Mailbox;

namespace AliroUd::Mailbox::Test {
namespace {

std::array<MailboxRecord, kMaxCredentials> sSlots{};
std::array<bool, kMaxCredentials> sSlotPresent{};

FaultPoint sArmedFault{ FaultPoint::None };

bool ConsumeFaultIfArmed(FaultPoint point)
{
	if (sArmedFault == point) {
		sArmedFault = FaultPoint::None;
		return true;
	}
	return false;
}

} // namespace

void ResetFakeMailboxPersistence()
{
	sSlots = {};
	sSlotPresent = {};
	sArmedFault = FaultPoint::None;
}

void ArmMailboxFault(FaultPoint point)
{
	sArmedFault = point;
}

} // namespace AliroUd::Mailbox::Test

namespace AliroUd::Mailbox::Persistence {

AliroError Init()
{
	return ALIRO_NO_ERROR;
}

AliroError LoadSlot(size_t slotIndex, MailboxRecord &out, bool &outPresent)
{
	outPresent = Test::sSlotPresent[slotIndex];
	if (outPresent) {
		out = Test::sSlots[slotIndex];
	}
	return ALIRO_NO_ERROR;
}

AliroError SaveSlot(size_t slotIndex, const MailboxRecord &value)
{
	if (Test::ConsumeFaultIfArmed(Test::FaultPoint::SaveSlot)) {
		return ALIRO_ERROR_INTERNAL;
	}
	Test::sSlots[slotIndex] = value;
	Test::sSlotPresent[slotIndex] = true;
	return ALIRO_NO_ERROR;
}

AliroError EraseSlot(size_t slotIndex)
{
	if (Test::ConsumeFaultIfArmed(Test::FaultPoint::EraseSlot)) {
		return ALIRO_ERROR_INTERNAL;
	}
	Test::sSlots[slotIndex] = MailboxRecord{};
	Test::sSlotPresent[slotIndex] = false;
	return ALIRO_NO_ERROR;
}

} // namespace AliroUd::Mailbox::Persistence
