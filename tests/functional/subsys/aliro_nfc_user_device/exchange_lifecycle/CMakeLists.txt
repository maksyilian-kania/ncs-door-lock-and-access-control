#
# Copyright (c) 2026 Nordic Semiconductor ASA
#
# SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
#

cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})

project(aliro_nfc_user_device_exchange_lifecycle)

set(ALIRO_UD_APP_SRC_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../../../../applications/aliro-nfc-user-device/src)
set(ALIRO_UD_WORKER_LIFECYCLE_TEST_SRC_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../worker_lifecycle/src)
set(ALIRO_UD_TEST_COMMON_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../common)
set(ALIRO_UD_STANDARD_INTEGRATION_TEST_SRC_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../standard_integration/src)

target_include_directories(app PRIVATE ${ALIRO_UD_APP_SRC_DIR} ${ALIRO_UD_TEST_COMMON_DIR}
					${ALIRO_UD_WORKER_LIFECYCLE_TEST_SRC_DIR} ${ALIRO_UD_STANDARD_INTEGRATION_TEST_SRC_DIR})

# Expedited Standard transactions continued through EXCHANGE, CONTROL FLOW,
# field loss, and session timeout, driven through the real worker thread
# and the real Aliro::UserDeviceStack against the real mailbox, OS timer,
# and transaction adapters. Only storage persistence, key-storage backends,
# the NFC-A transport, and the DK-hardware authorization indicator are
# fakes.
target_sources(app PRIVATE
  ${ALIRO_UD_APP_SRC_DIR}/platform/authorization/authorization_window.cpp
  ${ALIRO_UD_APP_SRC_DIR}/platform/authorization/authorization.cpp
  ${ALIRO_UD_TEST_COMMON_DIR}/fake_authorization_indicator.cpp
  ${ALIRO_UD_APP_SRC_DIR}/platform/nfc/nfc_worker.cpp
  ${ALIRO_UD_APP_SRC_DIR}/platform/os/os_mutex.cpp
  ${ALIRO_UD_APP_SRC_DIR}/platform/os/os_timer.cpp
  ${ALIRO_UD_APP_SRC_DIR}/platform/os/os_queue.cpp
  ${ALIRO_UD_APP_SRC_DIR}/platform/os/os_trusted_time.cpp
  ${ALIRO_UD_APP_SRC_DIR}/platform/os/os_logging.cpp
  ${ALIRO_UD_APP_SRC_DIR}/platform/os/transaction.cpp
  ${ALIRO_UD_APP_SRC_DIR}/platform/crypto/crypto.cpp
  ${ALIRO_UD_APP_SRC_DIR}/platform/crypto/certificate.cpp
  ${ALIRO_UD_APP_SRC_DIR}/platform/crypto/credential_signing.cpp
  ${ALIRO_UD_APP_SRC_DIR}/storage/credential/credential.cpp
  ${ALIRO_UD_APP_SRC_DIR}/storage/credential/credential_store.cpp
  ${ALIRO_UD_TEST_COMMON_DIR}/fake_credential_persistence.cpp
  ${ALIRO_UD_TEST_COMMON_DIR}/fake_key_backend.cpp
  ${ALIRO_UD_APP_SRC_DIR}/storage/document/document.cpp
  ${ALIRO_UD_APP_SRC_DIR}/storage/document/document_snapshots.cpp
  ${ALIRO_UD_APP_SRC_DIR}/storage/key/persistent_key.cpp
  ${ALIRO_UD_APP_SRC_DIR}/storage/key/persistent_key_store.cpp
  ${ALIRO_UD_TEST_COMMON_DIR}/fake_persistent_key_persistence.cpp
  ${ALIRO_UD_TEST_COMMON_DIR}/fake_persistent_key_backend.cpp
  ${ALIRO_UD_APP_SRC_DIR}/storage/mailbox/mailbox.cpp
  ${ALIRO_UD_APP_SRC_DIR}/storage/mailbox/mailbox_sessions.cpp
  ${ALIRO_UD_APP_SRC_DIR}/storage/mailbox/mailbox_store.cpp
  ${ALIRO_UD_TEST_COMMON_DIR}/fake_mailbox_persistence.cpp
  ${ALIRO_UD_WORKER_LIFECYCLE_TEST_SRC_DIR}/fake_nfc_interface.cpp
  ${ALIRO_UD_TEST_COMMON_DIR}/test_reader.cpp
  ${ALIRO_UD_STANDARD_INTEGRATION_TEST_SRC_DIR}/log_capture.c
  src/test_exchange_lifecycle.cpp
)

# nfc_worker.cpp calls into BeginCommandTiming()/EndCommandTiming()
# (APP_PLAN.md AWP7); mirrors the application's own CMakeLists.txt.
target_sources_ifdef(CONFIG_ALIRO_UD_TIMING_INSTRUMENTATION app PRIVATE
  ${ALIRO_UD_APP_SRC_DIR}/platform/nfc/command_timing.cpp
)
