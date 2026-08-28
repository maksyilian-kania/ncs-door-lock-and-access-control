/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include <zephyr/logging/log.h>

#include <aliro/user_device/interface.h>

/*
 * Placeholder implementation of Aliro::Interface::UserDevice::Authorization,
 * added only so the application links against the currently checked-out
 * stack (ncs-aliro WP5 introduces CredentialManager::EvaluateAuth0(), which
 * calls Authorization::GetState() unconditionally whenever authentication is
 * required). No DK button/LED backend exists yet: this stub fails closed
 * (every credential is always treated as requiring, but never granted,
 * authorization) rather than falsely authorizing a transaction. The real DK
 * button/authorization-window/LED implementation is added in AWP4
 * (APP_PLAN.md).
 *
 * GetState()'s error-bearing signature (WP5.5, decision D7) is adopted here
 * unchanged from the pre-WP5.5 `AuthorizationState GetState(handle)` stub;
 * AWP4 owns the real backend/failure-injection behavior.
 */
LOG_MODULE_REGISTER(aliro_ud_authorization, LOG_LEVEL_WRN);

namespace Aliro::Interface::UserDevice::Authorization {

AliroError GetState(::Aliro::UserDevice::CredentialHandle handle, ::Aliro::UserDevice::AuthorizationState &outState)
{
	ARG_UNUSED(handle);
	/*
	 * Fail closed: no authorization window can ever be granted yet.
	 * `Required` (not `Denied`) preserves this stub's pre-WP5.5 meaning
	 * exactly ("needed, never yet granted") rather than the stronger
	 * "explicitly denied" signal; per the contract doc, `outState` would
	 * only need to default to `Denied` on an *error* return, which this
	 * stub never produces.
	 */
	outState = ::Aliro::UserDevice::AuthorizationState::Required;
	return ALIRO_NO_ERROR;
}

void NotifyAuthenticationRequired(::Aliro::UserDevice::CredentialHandle handle)
{
	LOG_WRN("Authorization required for credential handle %u, no button/LED backend yet (AWP4)",
		static_cast<unsigned>(handle));
}

} // namespace Aliro::Interface::UserDevice::Authorization
