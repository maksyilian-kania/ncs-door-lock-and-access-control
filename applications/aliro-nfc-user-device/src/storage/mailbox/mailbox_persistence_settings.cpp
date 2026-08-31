/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include "mailbox_persistence.h"

#include <zephyr/logging/log.h>
#include <zephyr/settings/settings.h>

#include <cstdio>

LOG_MODULE_DECLARE(aliro_ud_mailbox, CONFIG_ALIRO_UD_MAILBOX_LOG_LEVEL);

/*
 * Real Zephyr settings-backed implementation of mailbox_persistence.h.
 * Same direct-key-query approach as
 * storage/credential/credential_persistence_settings.cpp (bypasses the
 * settings_handler registration mechanism): a small, fixed set of
 * well-known keys, one fixed-size POD blob per key, nothing outside this
 * application ever reads them.
 */
namespace AliroUd::Mailbox::Persistence {
namespace {

int SlotKey(size_t slotIndex, char *buf, size_t bufLen)
{
	return snprintf(buf, bufLen, "aliro_ud/mbox/%zu", slotIndex);
}

} // namespace

AliroError Init()
{
	/*
	 * settings_subsys_init() is idempotent (Zephyr settings tracks its
	 * own initialization); storage/credential's Init() already calls it
	 * during boot sequencing (src/main.cpp), but this module must not
	 * assume initialization order relative to that call.
	 */
	const int rc = settings_subsys_init();

	if (rc != 0) {
		LOG_ERR("settings_subsys_init() failed: %d", rc);
		return ALIRO_ERROR_INTERNAL;
	}

	return ALIRO_NO_ERROR;
}

AliroError LoadSlot(size_t slotIndex, MailboxRecord &out, bool &outPresent)
{
	char key[24];
	SlotKey(slotIndex, key, sizeof(key));

	MailboxRecord loaded{};
	const ssize_t rc = settings_load_one(key, &loaded, sizeof(loaded));

	if (rc == -ENOENT) {
		outPresent = false;
		return ALIRO_NO_ERROR;
	}

	if (rc < 0) {
		LOG_ERR("settings_load_one(%s) failed: %zd", key, rc);
		return ALIRO_ERROR_INTERNAL;
	}

	if (static_cast<size_t>(rc) != sizeof(loaded)) {
		LOG_ERR("settings_load_one(%s) returned unexpected length %zd (expected %zu)", key, rc,
			sizeof(loaded));
		return ALIRO_ERROR_INTERNAL;
	}

	out = loaded;
	outPresent = true;
	return ALIRO_NO_ERROR;
}

AliroError SaveSlot(size_t slotIndex, const MailboxRecord &value)
{
	char key[24];
	SlotKey(slotIndex, key, sizeof(key));

	const int rc = settings_save_one(key, &value, sizeof(value));

	if (rc != 0) {
		LOG_ERR("settings_save_one(%s) failed: %d", key, rc);
		return ALIRO_ERROR_INTERNAL;
	}

	return ALIRO_NO_ERROR;
}

AliroError EraseSlot(size_t slotIndex)
{
	char key[24];
	SlotKey(slotIndex, key, sizeof(key));

	const int rc = settings_delete(key);

	if (rc != 0) {
		LOG_ERR("settings_delete(%s) failed: %d", key, rc);
		return ALIRO_ERROR_INTERNAL;
	}

	return ALIRO_NO_ERROR;
}

} // namespace AliroUd::Mailbox::Persistence
