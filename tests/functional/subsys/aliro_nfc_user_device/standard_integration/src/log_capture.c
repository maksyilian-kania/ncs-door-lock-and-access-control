/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include "log_capture.h"

#include <zephyr/logging/log_backend.h>
#include <zephyr/logging/log_output.h>
#include <zephyr/spinlock.h>

#include <string.h>

#define LINE_CAPACITY 256
#define RECORD_CAPACITY 32

static const char kFilter[] = "Transaction result:";

static struct k_spinlock sLock;
static char sLine[LINE_CAPACITY];
static size_t sLineLength;
static char sRecords[RECORD_CAPACITY][LINE_CAPACITY];
static size_t sRecordCount;

static int CaptureOutput(uint8_t *data, size_t length, void *context)
{
	ARG_UNUSED(context);

	for (size_t i = 0; i < length && sLineLength < LINE_CAPACITY - 1; ++i) {
		sLine[sLineLength++] = (char)data[i];
	}
	return (int)length;
}

static uint8_t sOutputBuffer[64];
LOG_OUTPUT_DEFINE(sCaptureOutput, CaptureOutput, sOutputBuffer, sizeof(sOutputBuffer));

static void Process(const struct log_backend *const backend, union log_msg_generic *msg)
{
	ARG_UNUSED(backend);

	k_spinlock_key_t key = k_spin_lock(&sLock);

	sLineLength = 0;
	log_output_msg_process(&sCaptureOutput, &msg->log, 0);
	log_output_flush(&sCaptureOutput);
	sLine[sLineLength] = '\0';

	if (strstr(sLine, kFilter) != NULL && sRecordCount < RECORD_CAPACITY) {
		memcpy(sRecords[sRecordCount], sLine, sLineLength + 1);
		sRecordCount++;
	}

	k_spin_unlock(&sLock, key);
}

static void Panic(const struct log_backend *const backend)
{
	ARG_UNUSED(backend);
}

static const struct log_backend_api sCaptureApi = {
	.process = Process,
	.panic = Panic,
};

LOG_BACKEND_DEFINE(aliro_ud_test_log_capture, sCaptureApi, true);

void log_capture_reset(void)
{
	k_spinlock_key_t key = k_spin_lock(&sLock);
	sRecordCount = 0;
	k_spin_unlock(&sLock, key);
}

size_t log_capture_count(const char *needle)
{
	size_t count = 0;
	k_spinlock_key_t key = k_spin_lock(&sLock);
	for (size_t i = 0; i < sRecordCount; ++i) {
		if (strstr(sRecords[i], needle) != NULL) {
			count++;
		}
	}
	k_spin_unlock(&sLock, key);
	return count;
}

size_t log_capture_total(void)
{
	k_spinlock_key_t key = k_spin_lock(&sLock);
	const size_t count = sRecordCount;
	k_spin_unlock(&sLock, key);
	return count;
}
