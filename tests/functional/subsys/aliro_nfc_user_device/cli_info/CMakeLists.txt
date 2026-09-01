#
# Copyright (c) 2026 Nordic Semiconductor ASA
#
# SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
#

cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})

project(aliro_nfc_user_device_cli_info)

set(ALIRO_UD_APP_SRC_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../../../../applications/aliro-nfc-user-device/src)
set(ALIRO_UD_WORKER_LIFECYCLE_TEST_SRC_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../worker_lifecycle/src)
set(ALIRO_UD_TEST_COMMON_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../common)

target_include_directories(app PRIVATE ${ALIRO_UD_APP_SRC_DIR} ${ALIRO_UD_TEST_COMMON_DIR})

# Real cli.cpp under test (APP_PLAN.md AWP2), plus the real platform/nfc and
# platform/os code it reads state from ("info") and the real Aliro
# UserDeviceStack it links against. nfc_transport.cpp (nfc_t4t_lib hardware
# glue) is excluded, substituted by the worker_lifecycle test's
# fake_nfc_interface.cpp, exactly as in that test.
target_sources(app PRIVATE
  ${ALIRO_UD_APP_SRC_DIR}/cli/cli.cpp
  ${ALIRO_UD_APP_SRC_DIR}/platform/nfc/nfc_worker.cpp
  ${ALIRO_UD_APP_SRC_DIR}/platform/os/os_mutex.cpp
  ${ALIRO_UD_APP_SRC_DIR}/platform/os/os_timer.cpp
  ${ALIRO_UD_APP_SRC_DIR}/platform/os/os_queue.cpp
  ${ALIRO_UD_APP_SRC_DIR}/platform/os/os_trusted_time.cpp
  ${ALIRO_UD_APP_SRC_DIR}/platform/os/os_logging.cpp
  ${ALIRO_UD_APP_SRC_DIR}/platform/os/app_status.cpp
  ${ALIRO_UD_APP_SRC_DIR}/platform/os/transaction.cpp
  # Aliro::Interface::UserDevice::Credential/Trust adapter (AWP3) and its
  # real credential_store.cpp engine, with in-memory fakes standing in for
  # the hardware-backed persistence/key backends (native_sim has neither);
  # see tests/functional/subsys/aliro_nfc_user_device/common/.
  ${ALIRO_UD_APP_SRC_DIR}/storage/credential/credential.cpp
  ${ALIRO_UD_APP_SRC_DIR}/storage/credential/credential_store.cpp
  ${ALIRO_UD_TEST_COMMON_DIR}/fake_credential_persistence.cpp
  ${ALIRO_UD_TEST_COMMON_DIR}/fake_key_backend.cpp
  # Aliro::Interface::UserDevice::Mailbox implementation (AWP6) and the CLI's
  # "aliro-ud mailbox" commands over it.
  ${ALIRO_UD_APP_SRC_DIR}/storage/mailbox/mailbox.cpp
  ${ALIRO_UD_APP_SRC_DIR}/storage/mailbox/mailbox_sessions.cpp
  ${ALIRO_UD_APP_SRC_DIR}/storage/mailbox/mailbox_store.cpp
  ${ALIRO_UD_TEST_COMMON_DIR}/fake_mailbox_persistence.cpp
  # Aliro::Interface::UserDevice::Authorization adapter (AWP4), same
  # real-window/fake-indicator split as worker_lifecycle.
  ${ALIRO_UD_APP_SRC_DIR}/platform/authorization/authorization.cpp
  ${ALIRO_UD_APP_SRC_DIR}/platform/authorization/authorization_window.cpp
  ${ALIRO_UD_TEST_COMMON_DIR}/fake_authorization_indicator.cpp
  ${ALIRO_UD_WORKER_LIFECYCLE_TEST_SRC_DIR}/fake_nfc_interface.cpp
  src/test_cli_info.cpp
)

# cli.cpp's "aliro-ud timing" commands and nfc_worker.cpp both call into
# BeginCommandTiming()/EndCommandTiming()/GetCommandTimingSnapshot()/
# ResetCommandTimingStats() (APP_PLAN.md AWP7); mirrors the application's
# own CMakeLists.txt (see worker_lifecycle's copy of this comment for the
# full rationale).
target_sources_ifdef(CONFIG_ALIRO_UD_TIMING_INSTRUMENTATION app PRIVATE
  ${ALIRO_UD_APP_SRC_DIR}/platform/nfc/command_timing.cpp
)
