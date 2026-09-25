/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include "fake_persistent_key_persistence.h"

#include <storage/key/persistent_key_persistence.h>

#include <array>

using namespace AliroUd::PersistentKey;

namespace AliroUd::PersistentKey::Test {
namespace {

std::array<Record, kMaxRecords> sSlots{};
std::array<bool, kMaxRecords> sSlotPresent{};

} // namespace

void ResetFakePersistentKeyPersistence()
{
	sSlots = {};
	sSlotPresent = {};
}

} // namespace AliroUd::PersistentKey::Test

namespace AliroUd::PersistentKey::Persistence {

AliroError Init()
{
	return ALIRO_NO_ERROR;
}

AliroError LoadRecord(size_t slotIndex, Record &out, bool &outPresent)
{
	outPresent = Test::sSlotPresent[slotIndex];
	if (outPresent) {
		out = Test::sSlots[slotIndex];
	}
	return ALIRO_NO_ERROR;
}

AliroError SaveRecord(size_t slotIndex, const Record &value)
{
	Test::sSlots[slotIndex] = value;
	Test::sSlotPresent[slotIndex] = true;
	return ALIRO_NO_ERROR;
}

AliroError EraseRecord(size_t slotIndex)
{
	Test::sSlots[slotIndex] = Record{};
	Test::sSlotPresent[slotIndex] = false;
	return ALIRO_NO_ERROR;
}

} // namespace AliroUd::PersistentKey::Persistence
