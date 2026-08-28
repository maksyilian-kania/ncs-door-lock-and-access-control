/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include "credential_persistence.h"

#include <zephyr/logging/log.h>
#include <zephyr/settings/settings.h>

#include <cstdio>

LOG_MODULE_DECLARE(aliro_ud_credential, CONFIG_ALIRO_UD_CREDENTIAL_LOG_LEVEL);

/*
 * Real Zephyr settings-backed implementation of credential_persistence.h.
 * Bypasses the settings_handler registration mechanism entirely:
 * settings_load_one()/settings_save_one()/settings_delete() query the
 * backend (NVS or ZMS, selected by Kconfig.defconfig) directly by key, which
 * is sufficient for this small, fixed set of well-known records. Every
 * record is a fixed-size POD blob; no TLV/CBOR encoding is needed since
 * nothing outside this application ever reads these keys.
 */
namespace AliroUd::Credential::Persistence {
namespace {

int SlotKey(size_t slotIndex, char *buf, size_t bufLen)
{
	return snprintf(buf, bufLen, "aliro_ud/cred/%zu", slotIndex);
}

constexpr const char *kJournalKey{ "aliro_ud/journal" };
constexpr const char *kPreferredKey{ "aliro_ud/preferred" };

template <typename T> AliroError LoadRecord(const char *key, T &out, bool &outPresent)
{
	T loaded{};
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

template <typename T> AliroError SaveRecord(const char *key, const T &value)
{
	const int rc = settings_save_one(key, &value, sizeof(value));

	if (rc != 0) {
		LOG_ERR("settings_save_one(%s) failed: %d", key, rc);
		return ALIRO_ERROR_INTERNAL;
	}

	return ALIRO_NO_ERROR;
}

AliroError EraseRecord(const char *key)
{
	const int rc = settings_delete(key);

	if (rc != 0) {
		LOG_ERR("settings_delete(%s) failed: %d", key, rc);
		return ALIRO_ERROR_INTERNAL;
	}

	return ALIRO_NO_ERROR;
}

} // namespace

AliroError Init()
{
	const int rc = settings_subsys_init();

	if (rc != 0) {
		LOG_ERR("settings_subsys_init() failed: %d", rc);
		return ALIRO_ERROR_INTERNAL;
	}

	return ALIRO_NO_ERROR;
}

AliroError LoadSlot(size_t slotIndex, PersistedCredential &out, bool &outPresent)
{
	char key[24];
	SlotKey(slotIndex, key, sizeof(key));
	return LoadRecord(key, out, outPresent);
}

AliroError SaveSlot(size_t slotIndex, const PersistedCredential &value)
{
	char key[24];
	SlotKey(slotIndex, key, sizeof(key));
	return SaveRecord(key, value);
}

AliroError EraseSlot(size_t slotIndex)
{
	char key[24];
	SlotKey(slotIndex, key, sizeof(key));
	return EraseRecord(key);
}

AliroError LoadJournal(JournalRecord &out, bool &outPresent)
{
	return LoadRecord(kJournalKey, out, outPresent);
}

AliroError SaveJournal(const JournalRecord &value)
{
	return SaveRecord(kJournalKey, value);
}

AliroError EraseJournal()
{
	return EraseRecord(kJournalKey);
}

AliroError LoadPreferredTable(PreferredTable &out)
{
	bool present{ false };
	const auto error = LoadRecord(kPreferredKey, out, present);

	if (error != ALIRO_NO_ERROR) {
		return error;
	}

	if (!present) {
		out = PreferredTable{};
	}

	return ALIRO_NO_ERROR;
}

AliroError SavePreferredTable(const PreferredTable &value)
{
	return SaveRecord(kPreferredKey, value);
}

} // namespace AliroUd::Credential::Persistence
