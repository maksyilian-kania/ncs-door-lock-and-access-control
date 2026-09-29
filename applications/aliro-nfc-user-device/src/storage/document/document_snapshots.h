/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#pragma once

#include <aliro/errors.h>
#include <aliro/types.h>
#include <aliro/user_device/types.h>

/**
 * @brief Open-snapshot engine for `Aliro::Interface::UserDevice::Document`.
 *
 * `Open()` copies a credential's provisioned Access or Revocation Document
 * bytes into a fixed-capacity open-snapshot table at open time, so the
 * snapshot stays immutable to any later provisioning change until the
 * matching `Close()` (interface.h's "immutable snapshot" contract). Mirrors
 * `storage/mailbox/mailbox_sessions.h`'s shape.
 */
namespace AliroUd::Document::Snapshots {

/** @copydoc ::Aliro::Interface::UserDevice::Document::Open */
AliroError Open(::Aliro::UserDevice::CredentialHandle handle, ::Aliro::AccessDocumentTypes::DocumentType type,
		 ::Aliro::UserDevice::DocumentSnapshotHandle &outSnapshot);

/** @copydoc ::Aliro::Interface::UserDevice::Document::GetSize */
AliroError GetSize(::Aliro::UserDevice::DocumentSnapshotHandle snapshot, size_t &outSize);

/** @copydoc ::Aliro::Interface::UserDevice::Document::Read */
AliroError Read(::Aliro::UserDevice::DocumentSnapshotHandle snapshot, size_t offset, uint8_t *outData,
		 size_t length);

/** @copydoc ::Aliro::Interface::UserDevice::Document::Close */
void Close(::Aliro::UserDevice::DocumentSnapshotHandle snapshot);

/** @brief Number of currently open snapshots. Test/diagnostic use only. */
size_t GetOpenSnapshotCount();

} // namespace AliroUd::Document::Snapshots
