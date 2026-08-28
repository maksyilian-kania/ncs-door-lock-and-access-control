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
 */
LOG_MODULE_REGISTER(aliro_ud_authorization, LOG_LEVEL_WRN);

namespace Aliro::Interface::UserDevice::Authorization {

::Aliro::UserDevice::AuthorizationState GetState(::Aliro::UserDevice::CredentialHandle handle)
{
	ARG_UNUSED(handle);
	/* Fail closed: no authorization window can ever be granted yet. */
	return ::Aliro::UserDevice::AuthorizationState::Required;
}

void NotifyAuthenticationRequired(::Aliro::UserDevice::CredentialHandle handle)
{
	LOG_WRN("Authorization required for credential handle %u, no button/LED backend yet (AWP4)",
		static_cast<unsigned>(handle));
}

} // namespace Aliro::Interface::UserDevice::Authorization
