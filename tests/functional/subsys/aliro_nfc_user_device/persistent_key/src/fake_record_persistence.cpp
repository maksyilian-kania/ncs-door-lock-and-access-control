/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include "fake_record_persistence.h"
#include "fake_power_loss.h"

#include <storage/key/persistent_key_persistence.h>

#include <algorithm>
#include <cstdint>
#include <cstring>

namespace AliroUd::PersistentKey::FakeStorage {
namespace {

struct Slot {
	bool mPresent{ false };
	std::array<uint8_t, sizeof(Record)> mBytes{};
};

constexpr size_t kNoSlot{ SIZE_MAX };

std::array<Slot, kMaxRecords> sSlots{};
bool sFailNextSave{ false };
bool sFailNextErase{ false };
bool sFailNextInit{ false };
size_t sFailNextLoadSlot{ kNoSlot };
size_t sWriteCount{ 0 };

} // namespace

void Reset()
{
	sSlots = {};
	sFailNextSave = false;
	sFailNextErase = false;
	sFailNextInit = false;
	sFailNextLoadSlot = kNoSlot;
	sWriteCount = 0;
}

void FailNextInit()
{
	sFailNextInit = true;
}

void FailNextLoad(size_t slotIndex)
{
	sFailNextLoadSlot = slotIndex;
}

bool ReadFaultArmed()
{
	return sFailNextInit || sFailNextLoadSlot != kNoSlot;
}

void FailNextSave()
{
	sFailNextSave = true;
}

void FailNextErase()
{
	sFailNextErase = true;
}

void Preload(size_t slotIndex, const Record &record)
{
	std::memcpy(sSlots[slotIndex].mBytes.data(), &record, sizeof(Record));
	sSlots[slotIndex].mPresent = true;
}

Image Capture()
{
	Image image{};
	size_t offset = 0;
	for (const auto &slot : sSlots) {
		image[offset++] = slot.mPresent ? 1 : 0;
		std::copy(slot.mBytes.begin(), slot.mBytes.end(), image.begin() + offset);
		offset += slot.mBytes.size();
	}
	return image;
}

size_t PresentCount()
{
	return static_cast<size_t>(
		std::count_if(sSlots.begin(), sSlots.end(), [](const Slot &slot) { return slot.mPresent; }));
}

size_t WriteCount()
{
	return sWriteCount;
}

bool ImageContains(const uint8_t *bytes, size_t length)
{
	const Image image = Capture();
	return std::search(image.begin(), image.end(), bytes, bytes + length) != image.end();
}

} // namespace AliroUd::PersistentKey::FakeStorage

namespace AliroUd::PersistentKey::Persistence {

using namespace FakeStorage;

AliroError Init()
{
	if (sFailNextInit) {
		sFailNextInit = false;
		return ALIRO_ERROR_INTERNAL;
	}
	return ALIRO_NO_ERROR;
}

AliroError LoadRecord(size_t slotIndex, Record &out, bool &outPresent)
{
	if (sFailNextLoadSlot == slotIndex) {
		sFailNextLoadSlot = kNoSlot;
		outPresent = false;
		return ALIRO_ERROR_INTERNAL;
	}

	outPresent = sSlots[slotIndex].mPresent;
	if (outPresent) {
		std::memcpy(&out, sSlots[slotIndex].mBytes.data(), sizeof(Record));
	}
	return ALIRO_NO_ERROR;
}

AliroError SaveRecord(size_t slotIndex, const Record &value)
{
	if (sFailNextSave) {
		sFailNextSave = false;
		return ALIRO_ERROR_INTERNAL;
	}
	const FakePower::Outcome outcome = FakePower::NextDurableWrite(FakePower::Mutation::SaveRecord, slotIndex);
	if (outcome == FakePower::Outcome::Refuse) {
		return ALIRO_ERROR_INTERNAL;
	}

	std::memcpy(sSlots[slotIndex].mBytes.data(), &value, sizeof(Record));
	sSlots[slotIndex].mPresent = true;
	++sWriteCount;
	return outcome == FakePower::Outcome::LandThenFail ? ALIRO_ERROR_INTERNAL : ALIRO_NO_ERROR;
}

AliroError EraseRecord(size_t slotIndex)
{
	if (sFailNextErase) {
		sFailNextErase = false;
		return ALIRO_ERROR_INTERNAL;
	}
	const FakePower::Outcome outcome = FakePower::NextDurableWrite(FakePower::Mutation::EraseRecord, slotIndex);
	if (outcome == FakePower::Outcome::Refuse) {
		return ALIRO_ERROR_INTERNAL;
	}

	sSlots[slotIndex] = Slot{};
	++sWriteCount;
	return outcome == FakePower::Outcome::LandThenFail ? ALIRO_ERROR_INTERNAL : ALIRO_NO_ERROR;
}

} // namespace AliroUd::PersistentKey::Persistence
