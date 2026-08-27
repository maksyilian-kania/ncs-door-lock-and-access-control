/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include <zephyr/logging/log.h>

#include <cstdarg>
#include <cstddef>
#include <cstdint>

LOG_MODULE_REGISTER(aliro_ud_stack, CONFIG_NCS_ALIRO_USER_DEVICE_LOG_LEVEL_VALUE);

/*
 * Aliro::Interface::Logging::Log()/LogHexdump() are a role-neutral platform
 * utility (PLAN.md "ExistingNeutralUtilities"): stack/src/aliro/log.h
 * declares and calls them unconditionally, for every ALIRO_UD_LOG and
 * ALIRO_LOG call site, independent of which role (Reader/User Device) is
 * built in. door-lock-and-access-control-app already ships a Reader-scoped
 * implementation gated "depends on NCS_ALIRO"
 * (subsys/aliro/interface_impl/log), which is unusable in a User-Device-only
 * build (NCS_ALIRO is not selected, so CONFIG_NCS_ALIRO_LOG_LEVEL_VALUE does
 * not exist). This is the User Device-scoped equivalent, gated on
 * CONFIG_NCS_ALIRO_USER_DEVICE_LOG_LEVEL_VALUE instead.
 *
 * The public aliro/interface.h only declares this namespace when
 * CONFIG_NCS_ALIRO_LOG_LEVEL_VALUE is greater than 0 (Reader-only), so it is
 * not included here; the declarations below just need to match the private
 * stack/src/aliro/log.h ones that user_device/log.h's logging macros
 * actually call.
 */
namespace Aliro::Interface::Logging {

void Log(uint8_t platformLogLevel, const char *logFormat, ...)
{
#if defined(CONFIG_LOG) && !defined(CONFIG_LOG_MODE_MINIMAL)

	if (platformLogLevel > CONFIG_NCS_ALIRO_USER_DEVICE_LOG_LEVEL_VALUE) {
		return;
	}

	va_list paramList;
	va_start(paramList, logFormat);
	log_generic(platformLogLevel, logFormat, paramList);
	va_end(paramList);

#else // defined(CONFIG_LOG) && !defined(CONFIG_LOG_MODE_MINIMAL)

	ARG_UNUSED(platformLogLevel);
	ARG_UNUSED(logFormat);

#endif // defined(CONFIG_LOG) && !defined(CONFIG_LOG_MODE_MINIMAL)
}

void LogHexdump(uint8_t platformLogLevel, const void *data, size_t size, const char *str)
{
#if defined(CONFIG_LOG)

	/*
	 * Zephyr's LOG_LEVEL_{ERR,WRN,INF,DBG} share Aliro's platformLogLevel
	 * numbering (1-4), matching stack/src/aliro/log.h.
	 */
	switch (platformLogLevel) {
	case LOG_LEVEL_ERR:
		LOG_HEXDUMP_ERR(data, size, str);
		break;
	case LOG_LEVEL_WRN:
		LOG_HEXDUMP_WRN(data, size, str);
		break;
	case LOG_LEVEL_INF:
		LOG_HEXDUMP_INF(data, size, str);
		break;
	case LOG_LEVEL_DBG:
		LOG_HEXDUMP_DBG(data, size, str);
		break;
	default:
		break;
	}

#else // defined(CONFIG_LOG)

	ARG_UNUSED(platformLogLevel);
	ARG_UNUSED(data);
	ARG_UNUSED(size);
	ARG_UNUSED(str);

#endif // defined(CONFIG_LOG)
}

} // namespace Aliro::Interface::Logging
