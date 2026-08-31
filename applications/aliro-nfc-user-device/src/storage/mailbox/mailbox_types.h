/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#pragma once

#include "storage/credential/credential_types.h"

#include <aliro/user_device/types.h>

#include <array>
#include <cstdint>

/**
 * @brief Per-credential mailbox byte-storage data model (APP_PLAN.md AWP6).
 *
 * Aliro 1.0 Specification, section 8.3.1.15, pages 60-61: the mailbox size
 * and access rights are provisioned per Access Credential (AWP3,
 * `AliroUd::Credential::MailboxConfig`); this module owns only the
 * committed byte content behind that configuration, plus the session-scoped
 * staged-mutation state used by the `Aliro::Interface::UserDevice::Mailbox`
 * contract (snapshot/read/stage/commit/rollback/close).
 *
 * There is no `ncs-aliro` public contract binding a `MailboxHandle` to a
 * `CredentialHandle` (the checked-out public User Device headers and the
 * stack's own User Device sources never construct or consume a
 * `MailboxHandle` value); this is an application-only design decision,
 * documented in `docs/evidence.md`: since Phase 1 provisions at most one
 * mailbox per Access Credential, `MailboxHandle` numerically equals its
 * owning `CredentialHandle` (see `HandleForCredential()`/
 * `CredentialForHandle()` in `mailbox_store.h`).
 */
namespace AliroUd::Mailbox {

/** @brief Maximum provisionable mailbox size, in bytes (shared with `storage/credential`'s Kconfig). */
constexpr size_t kMaxSizeBytes{ AliroUd::Credential::kMailboxMaxSizeBytes };

/** @brief Fixed number of mailbox slots: one per possible credential slot. */
constexpr size_t kMaxCredentials{ AliroUd::Credential::kMaxCredentials };

/** @brief Maximum number of concurrently open mailbox sessions (`OpenSnapshot()` without a matching `Close()`). */
constexpr size_t kMaxSessions{ CONFIG_ALIRO_UD_MAILBOX_MAX_SESSIONS };

/**
 * @brief Committed per-credential mailbox byte storage.
 *
 * `mInitialized` is false until the mailbox has been explicitly
 * initialized (`Store::Initialize()`) at least once; Aliro 1.0
 * Specification, Appendix 18 ("When no data is present in the mailbox, all
 * bytes contained in the mailbox SHALL be set to 0x00") is satisfied by
 * `mData`'s default zero-initialization either way.
 */
struct MailboxRecord {
	bool mInitialized{ false };
	std::array<uint8_t, kMaxSizeBytes> mData{};
};

} // namespace AliroUd::Mailbox
