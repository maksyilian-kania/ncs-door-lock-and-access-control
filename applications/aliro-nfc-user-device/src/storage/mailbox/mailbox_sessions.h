/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#pragma once

#include "mailbox_types.h"

#include <aliro/errors.h>
#include <aliro/user_device/interface.h>
#include <aliro/user_device/mailbox.h>
#include <aliro/user_device/types.h>

/**
 * @brief Reader-facing mailbox session engine implementing the semantics of
 * `Aliro::Interface::UserDevice::Mailbox` (APP_PLAN.md AWP6): snapshot
 * open, bounds/permission-checked read, staged write/set, atomic commit,
 * rollback, and close.
 *
 * `mailbox.cpp` is a thin adapter translating this API 1:1 to the
 * `Aliro::Interface::UserDevice::Mailbox` contract (same split as
 * `storage/credential/credential.cpp` over `credential_store.cpp`), kept
 * separate so host tests can exercise the session engine directly by its
 * own (non-opaque, non-`Aliro::Interface`) name.
 *
 * Every read observes only committed bytes (`AliroUd::Mailbox::Store`);
 * `StageWrite()`/`StageSet()` mutate only this session's private staged
 * shadow, invisible to `Read()` and to every other session, until
 * `Commit()` atomically applies every staged byte to committed storage in
 * one `Store::ApplyDirtyBytes()` call. `Rollback()`/`Close()` without a
 * commit leave committed bytes completely unchanged.
 *
 * `Read()`/`StageWrite()`/`StageSet()` are gated by the credential's
 * provisioned `MailboxPermissions` (Reader access rights); overflow-safe
 * bounds are re-checked against the *live* provisioned size on every call,
 * so a credential update that shrinks the mailbox mid-session is reflected
 * immediately.
 */
namespace AliroUd::Mailbox::Sessions {

using SessionHandle = ::Aliro::Interface::UserDevice::Mailbox::SessionHandle;
constexpr SessionHandle kInvalidSessionHandle{ ::Aliro::Interface::UserDevice::Mailbox::kInvalidSessionHandle };

/*
 * WP7 stack impact (see docs/wp7_stack_impact.md), amendment A6: a
 * conforming backend supports at most one open snapshot per mailbox.
 * OpenSnapshot() returns ALIRO_INVALID_STATE (distinct from
 * ALIRO_INVALID_ARGUMENT for an invalid/absent mailbox) if a session is
 * already open for the requested handle.
 */
AliroError OpenSnapshot(::Aliro::UserDevice::MailboxHandle handle, SessionHandle &outSession);
AliroError Read(SessionHandle session, size_t offset, uint8_t *outData, size_t length);
AliroError StageWrite(SessionHandle session, size_t offset, const uint8_t *data, size_t length);
/*
 * WP7 stack impact, amendment A1 (breaking change): fills [offset, offset +
 * length) with a single repeated byte (Table 8-16 0x95 set request), not a
 * full-mailbox multi-byte buffer as before.
 */
AliroError StageSet(SessionHandle session, size_t offset, size_t length, uint8_t value);
AliroError Commit(SessionHandle session);
AliroError Rollback(SessionHandle session);
void Close(SessionHandle session);

/** @brief Number of currently open sessions. Test/diagnostic use only. */
size_t GetOpenSessionCount();

} // namespace AliroUd::Mailbox::Sessions
