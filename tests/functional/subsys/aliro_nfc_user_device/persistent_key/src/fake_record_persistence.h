/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#pragma once

#include <storage/key/persistent_key_types.h>

#include <array>
#include <cstddef>
#include <cstdint>

/**
 * @brief In-memory `storage/key/persistent_key_persistence.h` that stores
 * each record verbatim as `sizeof(Record)` bytes, as the settings backend
 * does, so tests can compare and scan the exact persisted image. Survives
 * `AliroUd::PersistentKey::Store::Init()`, simulating a reboot.
 */
namespace AliroUd::PersistentKey::FakeStorage {

/** @brief Presence flag plus raw bytes of every slot. */
using Image = std::array<uint8_t, kMaxRecords * (1 + sizeof(Record))>;

/** @brief Clears every slot, counter, and armed fault (first boot). */
void Reset();

/** @brief Arms one failure of the next `Persistence::SaveRecord()`. */
void FailNextSave();

/** @brief Writes `record` into `slotIndex` directly, as if left by an earlier boot. */
void Preload(size_t slotIndex, const Record &record);

/** @brief Returns the current persisted image. */
Image Capture();

/** @brief Number of present slots. */
size_t PresentCount();

/** @brief Number of successful `SaveRecord()`/`EraseRecord()` calls. */
size_t WriteCount();

/** @brief Returns true if `length` consecutive bytes equal to `bytes` appear anywhere in the image. */
bool ImageContains(const uint8_t *bytes, size_t length);

} // namespace AliroUd::PersistentKey::FakeStorage
