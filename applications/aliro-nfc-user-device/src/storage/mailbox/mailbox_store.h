/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#pragma once

#include "mailbox_types.h"

#include <aliro/errors.h>
#include <aliro/user_device/mailbox.h>
#include <aliro/user_device/types.h>

#include <array>
#include <cstdint>

/**
 * @brief Per-credential mailbox committed byte-storage engine (APP_PLAN.md
 * AWP6).
 *
 * Owns the fixed-capacity mailbox slot table and its Zephyr settings/NVS
 * persistence (`mailbox_persistence.h`). This is Credential-Issuer-level
 * access: Aliro 1.0 Specification section 8.3.1.15, page 60, "The mailbox
 * content SHALL be readable and writeable by the Credential Issuer" — this
 * application's CLI plays that role for local provisioning (APP_PLAN.md
 * exclusions: "the development CLI provides local provisioning"), so every
 * function here is bounds-checked against the credential's provisioned
 * mailbox size but is **not** gated by the Reader-facing
 * `MailboxPermissions` (readable/writable) bits — those gate only the
 * Reader-driven `Aliro::Interface::UserDevice::Mailbox` session contract
 * implemented in `mailbox.cpp`/`mailbox_sessions.{h,cpp}`, which calls back
 * into this module for the actual committed-byte access.
 */
namespace AliroUd::Mailbox::Store {

/** @brief Maps an Access Credential handle to its mailbox handle (APP_PLAN.md AWP6 design decision; see mailbox_types.h). */
constexpr ::Aliro::UserDevice::MailboxHandle HandleForCredential(::Aliro::UserDevice::CredentialHandle credentialHandle)
{
	return static_cast<::Aliro::UserDevice::MailboxHandle>(credentialHandle);
}

/** @brief Inverse of `HandleForCredential()`. */
constexpr ::Aliro::UserDevice::CredentialHandle CredentialForHandle(::Aliro::UserDevice::MailboxHandle mailboxHandle)
{
	return static_cast<::Aliro::UserDevice::CredentialHandle>(mailboxHandle);
}

/**
 * @brief Initializes the persistence backend and loads every mailbox slot.
 *
 * Must be called exactly once at boot, after
 * `AliroUd::Credential::Store::Init()` (mailbox configuration lives in the
 * credential record) and before the NFC transport starts.
 */
AliroError Init();

/**
 * @brief Non-secret, provisioned mailbox configuration for a credential,
 * read live from `AliroUd::Credential::Store` (the single source of truth
 * for size/permissions; APP_PLAN.md AWP6: "Mailbox size and rights remain
 * part of the credential staging transaction from AWP3").
 */
struct Config {
	bool mConfigured{ false };
	uint32_t mSizeBytes{ 0 };
	::Aliro::UserDevice::MailboxPermissions mPermissions{};
};

/** @brief Gets the live mailbox configuration for a credential handle. `ALIRO_INVALID_ARGUMENT` if the credential does not exist or has no mailbox configured. */
AliroError GetConfig(::Aliro::UserDevice::CredentialHandle credentialHandle, Config &outConfig);

/**
 * @brief Ensures a configured mailbox has committed byte storage
 * (idempotent: a no-op success if already initialized), zero-filling it on
 * first use per Aliro 1.0 Specification, Appendix 18 ("When no data is
 * present in the mailbox, all bytes contained in the mailbox SHALL be set
 * to 0x00").
 */
AliroError Initialize(::Aliro::UserDevice::CredentialHandle credentialHandle);

/**
 * @brief Unconditionally re-zeroes a configured mailbox's committed byte
 * storage (APP_PLAN.md AWP6 CLI "reset"), even if already initialized.
 */
AliroError Reset(::Aliro::UserDevice::CredentialHandle credentialHandle);

/** @brief Whether a mailbox has been initialized (`Initialize()`/`Reset()` called at least once). */
bool IsInitialized(::Aliro::UserDevice::CredentialHandle credentialHandle);

/**
 * @brief Whether any committed byte differs from zero (Aliro 1.0
 * Specification, section 8.3.3.4.2, Table 8-11, Bit3: "if set, some data
 * different from zeroes is present in the mailbox").
 */
bool HasNonZeroData(::Aliro::UserDevice::CredentialHandle credentialHandle);

/**
 * @brief Credential-Issuer-level bounds-checked read of committed bytes
 * (used directly by the CLI; not gated by Reader `MailboxPermissions`).
 */
AliroError RawRead(::Aliro::UserDevice::CredentialHandle credentialHandle, size_t offset, uint8_t *outData,
		   size_t length);

/**
 * @brief Overwrites exactly the dirty byte ranges described by `data`/
 * `dirty` into a mailbox's committed storage in one persistence write
 * (the atomic-commit primitive used by the session layer's `Commit()`).
 *
 * @param data Full-size shadow buffer (`kMaxSizeBytes`); only bytes marked
 * `true` in `dirty` (also full-size) are applied.
 * @param dirty Per-byte dirty bitmap, same size as `data`.
 */
AliroError ApplyDirtyBytes(::Aliro::UserDevice::CredentialHandle credentialHandle,
			   const std::array<uint8_t, kMaxSizeBytes> &data,
			   const std::array<bool, kMaxSizeBytes> &dirty);

/** @brief Erases one credential's mailbox byte storage entirely (APP_PLAN.md AWP6: called on credential delete). Idempotent. */
AliroError EraseForCredential(::Aliro::UserDevice::CredentialHandle credentialHandle);

/** @brief Erases every credential's mailbox byte storage (APP_PLAN.md AWP6: called on credential factory reset). */
AliroError EraseAll();

} // namespace AliroUd::Mailbox::Store
