/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include "mailbox_sessions.h"

#include <aliro/user_device/interface.h>

/*
 * Thin adapter from Aliro::Interface::UserDevice::Mailbox to this
 * application's own mailbox_sessions engine (APP_PLAN.md AWP6). Every
 * function here does exactly one thing: forward to
 * AliroUd::Mailbox::Sessions. No storage, bounds, or permission logic
 * lives in this file; see mailbox_sessions.cpp/mailbox_store.cpp for that.
 */
namespace Aliro::Interface::UserDevice::Mailbox {

AliroError OpenSnapshot(::Aliro::UserDevice::MailboxHandle handle, SessionHandle &outSession)
{
	return AliroUd::Mailbox::Sessions::OpenSnapshot(handle, outSession);
}

AliroError Read(SessionHandle session, size_t offset, uint8_t *outData, size_t length)
{
	return AliroUd::Mailbox::Sessions::Read(session, offset, outData, length);
}

AliroError StageWrite(SessionHandle session, size_t offset, const uint8_t *data, size_t length)
{
	return AliroUd::Mailbox::Sessions::StageWrite(session, offset, data, length);
}

AliroError StageSet(SessionHandle session, const uint8_t *data, size_t length)
{
	return AliroUd::Mailbox::Sessions::StageSet(session, data, length);
}

AliroError Commit(SessionHandle session)
{
	return AliroUd::Mailbox::Sessions::Commit(session);
}

AliroError Rollback(SessionHandle session)
{
	return AliroUd::Mailbox::Sessions::Rollback(session);
}

void Close(SessionHandle session)
{
	AliroUd::Mailbox::Sessions::Close(session);
}

} // namespace Aliro::Interface::UserDevice::Mailbox
