/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include "document_snapshots.h"

#include "storage/credential/credential_store.h"

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <algorithm>
#include <array>

LOG_MODULE_REGISTER(aliro_ud_document, CONFIG_ALIRO_UD_DOCUMENT_LOG_LEVEL);

using namespace Aliro;
using namespace Aliro::UserDevice;
using namespace Aliro::AccessDocumentTypes;

/*
 * The public Document contract bounds one open snapshot to
 * CONFIG_NCS_ALIRO_USER_DEVICE_MAX_DOCUMENT_SNAPSHOT_BYTES, and AUTH1 opens
 * the Access and Revocation snapshots of one credential concurrently.
 */
static_assert(AliroUd::Credential::kDocumentMaxSizeBytes <= CONFIG_NCS_ALIRO_USER_DEVICE_MAX_DOCUMENT_SNAPSHOT_BYTES,
	      "CONFIG_ALIRO_UD_DOCUMENT_MAX_SIZE exceeds the stack's document snapshot bound");
static_assert(CONFIG_ALIRO_UD_MAX_OPEN_DOCUMENT_SNAPSHOTS >= 2,
	      "Access and Revocation snapshots of one session must be open concurrently");

namespace AliroUd::Document::Snapshots {
namespace {

struct Snapshot {
	bool mActive{ false };
	DocumentSnapshotHandle mHandle{ kInvalidDocumentSnapshotHandle };
	std::array<uint8_t, AliroUd::Credential::kDocumentMaxSizeBytes> mData{};
	uint32_t mLength{ 0 };
};

K_MUTEX_DEFINE(sMutex);
std::array<Snapshot, CONFIG_ALIRO_UD_MAX_OPEN_DOCUMENT_SNAPSHOTS> sSnapshots{};
DocumentSnapshotHandle sNextHandle{ 1 };

class Lock {
public:
	Lock() { k_mutex_lock(&sMutex, K_FOREVER); }
	~Lock() { k_mutex_unlock(&sMutex); }
	Lock(const Lock &) = delete;
	Lock &operator=(const Lock &) = delete;
};

/* Overflow-safe [offset, offset + length) <= size. */
bool InBounds(size_t offset, size_t length, size_t size)
{
	if (offset > size) {
		return false;
	}
	return length <= (size - offset);
}

Snapshot *FindLocked(DocumentSnapshotHandle handle)
{
	if (handle == kInvalidDocumentSnapshotHandle) {
		return nullptr;
	}

	for (auto &snapshot : sSnapshots) {
		if (snapshot.mActive && snapshot.mHandle == handle) {
			return &snapshot;
		}
	}
	return nullptr;
}

} // namespace

AliroError Open(CredentialHandle handle, DocumentType type, DocumentSnapshotHandle &outSnapshot)
{
	outSnapshot = kInvalidDocumentSnapshotHandle;
	if (type != DocumentType::Access && type != DocumentType::Revocation) {
		return ALIRO_INVALID_ARGUMENT;
	}

	Lock lock;

	AliroUd::Credential::PersistedCredential record{};
	const auto recordError = AliroUd::Credential::Store::GetFullRecord(handle, record);
	if (recordError != ALIRO_NO_ERROR) {
		return ALIRO_INVALID_ARGUMENT;
	}

	const AliroUd::Credential::OptionalDocument &document =
		(type == DocumentType::Access) ? record.mAccessDocument : record.mRevocationDocument;
	if (!document.mPresent) {
		return ALIRO_INVALID_ARGUMENT;
	}
	/* A persisted length beyond the buffer (e.g. corrupted storage) must not be read past. */
	if (document.mLength > document.mData.size()) {
		LOG_ERR("Stored document length %u exceeds capacity %zu", document.mLength, document.mData.size());
		return ALIRO_NO_MEMORY;
	}

	Snapshot *freeSlot{ nullptr };
	for (auto &snapshot : sSnapshots) {
		if (!snapshot.mActive) {
			freeSlot = &snapshot;
			break;
		}
	}

	if (freeSlot == nullptr) {
		LOG_WRN("No free document snapshot slot (capacity %zu)", sSnapshots.size());
		return ALIRO_NO_MEMORY;
	}

	*freeSlot = Snapshot{};
	freeSlot->mActive = true;
	freeSlot->mHandle = sNextHandle++;
	if (sNextHandle == kInvalidDocumentSnapshotHandle) {
		sNextHandle = 1;
	}
	freeSlot->mData = document.mData;
	freeSlot->mLength = document.mLength;

	outSnapshot = freeSlot->mHandle;
	return ALIRO_NO_ERROR;
}

AliroError GetSize(DocumentSnapshotHandle snapshot, size_t &outSize)
{
	outSize = 0;
	Lock lock;

	auto *found = FindLocked(snapshot);
	if (found == nullptr) {
		return ALIRO_INVALID_STATE;
	}

	outSize = found->mLength;
	return ALIRO_NO_ERROR;
}

AliroError Read(DocumentSnapshotHandle snapshot, size_t offset, uint8_t *outData, size_t length)
{
	Lock lock;

	auto *found = FindLocked(snapshot);
	if (found == nullptr) {
		return ALIRO_INVALID_STATE;
	}

	if (!InBounds(offset, length, found->mLength) || (outData == nullptr && length != 0)) {
		return ALIRO_INVALID_ARGUMENT;
	}

	std::copy_n(found->mData.begin() + offset, length, outData);
	return ALIRO_NO_ERROR;
}

void Close(DocumentSnapshotHandle snapshot)
{
	Lock lock;

	auto *found = FindLocked(snapshot);
	if (found == nullptr) {
		return;
	}

	*found = Snapshot{};
}

size_t GetOpenSnapshotCount()
{
	Lock lock;

	return static_cast<size_t>(
		std::count_if(sSnapshots.begin(), sSnapshots.end(), [](const Snapshot &s) { return s.mActive; }));
}

} // namespace AliroUd::Document::Snapshots
