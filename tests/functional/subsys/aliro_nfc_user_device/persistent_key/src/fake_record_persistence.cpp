/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include "fake_record_persistence.h"

#include <storage/key/persistent_key_persistence.h>

#include <algorithm>
#include <cstring>

namespace AliroUd::PersistentKey::FakeStorage {
namespace {

struct Slot {
	bool mPresent{ false };
	std::array<uint8_t, sizeof(Record)> mBytes{};
};

std::array<Slot, kMaxRecords> sSlots{};
bool sFailNextSave{ false };
size_t sWriteCount{ 0 };

} // namespace

void Reset()
{
	sSlots = {};
	sFailNextSave = false;
	sWriteCount = 0;
}

void FailNextSave()
{
	sFailNextSave = true;
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
	return ALIRO_NO_ERROR;
}

AliroError LoadRecord(size_t slotIndex, Record &out, bool &outPresent)
{
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

	std::memcpy(sSlots[slotIndex].mBytes.data(), &value, sizeof(Record));
	sSlots[slotIndex].mPresent = true;
	++sWriteCount;
	return ALIRO_NO_ERROR;
}

AliroError EraseRecord(size_t slotIndex)
{
	sSlots[slotIndex] = Slot{};
	++sWriteCount;
	return ALIRO_NO_ERROR;
}

} // namespace AliroUd::PersistentKey::Persistence
