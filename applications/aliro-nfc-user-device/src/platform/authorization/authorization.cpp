/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include "authorization_indicator.h"
#include "authorization_window.h"

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <aliro/user_device/interface.h>

/*
 * Aliro::Interface::UserDevice::Authorization backed by a DK button and
 * visible LED indication (APP_PLAN.md AWP4).
 *
 * GetState() only ever reads AliroUd::Authorization::GlobalWindow() and
 * returns synchronously - it never waits for a button press inside an NFC
 * transaction ("Do not wait inside the NFC transaction for a button press.
 * Return the synchronous Required state so the stack fails promptly").
 * Opening the window (a real button press, authorization_button.cpp, or the
 * "aliro-ud auth press" CLI test trigger) is entirely decoupled from this
 * call and only ever affects a *later* transaction.
 */
LOG_MODULE_REGISTER(aliro_ud_authorization, CONFIG_ALIRO_UD_AUTHORIZATION_LOG_LEVEL);

namespace Aliro::Interface::UserDevice::Authorization {

AliroError GetState(::Aliro::UserDevice::CredentialHandle handle, ::Aliro::UserDevice::AuthorizationState &outState)
{
	ARG_UNUSED(handle);

	/* WP5.5, decision D7: left at Denied unless a real state is produced below; this backend never errors. */
	outState = ::Aliro::UserDevice::AuthorizationState::Denied;

	outState = AliroUd::Authorization::GlobalWindow().GetState(k_uptime_get());
	return ALIRO_NO_ERROR;
}

void NotifyAuthenticationRequired(::Aliro::UserDevice::CredentialHandle handle)
{
	LOG_INF("Authorization required for credential handle %u; no valid button window, indicating",
		static_cast<unsigned>(handle));
	AliroUd::Authorization::Indicator::SetActive(true);
}

} // namespace Aliro::Interface::UserDevice::Authorization
