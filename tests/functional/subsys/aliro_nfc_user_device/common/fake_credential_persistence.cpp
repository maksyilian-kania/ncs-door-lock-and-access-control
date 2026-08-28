/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include "fake_credential_persistence.h"

#include <storage/credential/credential_persistence.h>

#include <array>

using namespace AliroUd::Credential;

namespace AliroUd::Credential::Test {
namespace {

std::array<PersistedCredential, kMaxCredentials> sSlots{};
std::array<bool, kMaxCredentials> sSlotPresent{};
JournalRecord sJournal{};
bool sJournalPresent{ false };
PreferredTable sPreferredTable{};

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

void ResetFakePersistence()
{
	sSlots = {};
	sSlotPresent = {};
	sJournal = JournalRecord{};
	sJournalPresent = false;
	sPreferredTable = PreferredTable{};
	sArmedFault = FaultPoint::None;
}

void ArmFault(FaultPoint point)
{
	sArmedFault = point;
}

bool IsFaultArmed()
{
	return sArmedFault != FaultPoint::None;
}

} // namespace AliroUd::Credential::Test

namespace AliroUd::Credential::Persistence {

AliroError Init()
{
	return ALIRO_NO_ERROR;
}

AliroError LoadSlot(size_t slotIndex, PersistedCredential &out, bool &outPresent)
{
	outPresent = Test::sSlotPresent[slotIndex];
	if (outPresent) {
		out = Test::sSlots[slotIndex];
	}
	return ALIRO_NO_ERROR;
}

AliroError SaveSlot(size_t slotIndex, const PersistedCredential &value)
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
	Test::sSlots[slotIndex] = PersistedCredential{};
	Test::sSlotPresent[slotIndex] = false;
	return ALIRO_NO_ERROR;
}

AliroError LoadJournal(JournalRecord &out, bool &outPresent)
{
	outPresent = Test::sJournalPresent;
	if (outPresent) {
		out = Test::sJournal;
	}
	return ALIRO_NO_ERROR;
}

AliroError SaveJournal(const JournalRecord &value)
{
	if (Test::ConsumeFaultIfArmed(Test::FaultPoint::SaveJournal)) {
		return ALIRO_ERROR_INTERNAL;
	}
	Test::sJournal = value;
	Test::sJournalPresent = true;
	return ALIRO_NO_ERROR;
}

AliroError EraseJournal()
{
	if (Test::ConsumeFaultIfArmed(Test::FaultPoint::EraseJournal)) {
		return ALIRO_ERROR_INTERNAL;
	}
	Test::sJournal = JournalRecord{};
	Test::sJournalPresent = false;
	return ALIRO_NO_ERROR;
}

AliroError LoadPreferredTable(PreferredTable &out)
{
	out = Test::sPreferredTable;
	return ALIRO_NO_ERROR;
}

AliroError SavePreferredTable(const PreferredTable &value)
{
	if (Test::ConsumeFaultIfArmed(Test::FaultPoint::SavePreferredTable)) {
		return ALIRO_ERROR_INTERNAL;
	}
	Test::sPreferredTable = value;
	return ALIRO_NO_ERROR;
}

} // namespace AliroUd::Credential::Persistence
