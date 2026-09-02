#
# Copyright (c) 2026 Nordic Semiconductor ASA
#
# SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
#

cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})

project(aliro_nfc_user_device_authorization)

set(ALIRO_UD_APP_SRC_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../../../../applications/aliro-nfc-user-device/src)
set(ALIRO_UD_WORKER_LIFECYCLE_TEST_SRC_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../worker_lifecycle/src)
set(ALIRO_UD_TEST_COMMON_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../common)

target_include_directories(app PRIVATE ${ALIRO_UD_APP_SRC_DIR} ${ALIRO_UD_TEST_COMMON_DIR}
					${ALIRO_UD_WORKER_LIFECYCLE_TEST_SRC_DIR})

# authorization_window.cpp (pure state machine) and authorization.cpp (the
# real Aliro::Interface::UserDevice::Authorization adapter) under test
# (APP_PLAN.md AWP4), plus the real worker/os/credential stack so
# test_authorization_e2e.cpp can drive a real AUTH0 command through the real
# Aliro::UserDeviceStack, exactly as worker_lifecycle/cli_info do.
# authorization_button.cpp/authorization_led.cpp (DK-hardware-only GPIO) are
# excluded, substituted by fake_authorization_indicator.cpp.
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
  ${ALIRO_UD_APP_SRC_DIR}/storage/credential/credential.cpp
  ${ALIRO_UD_APP_SRC_DIR}/storage/credential/credential_store.cpp
  ${ALIRO_UD_TEST_COMMON_DIR}/fake_credential_persistence.cpp
  ${ALIRO_UD_TEST_COMMON_DIR}/fake_key_backend.cpp
  # WP7 stack impact (see docs/wp7_stack_impact.md): the real
  # Aliro::UserDeviceStack now unconditionally calls
  # Aliro::Interface::UserDevice::Mailbox::* from
  # UserDeviceStateMachine::HandleAuth1()/HandleExchange() (WP7-S7), even
  # for SELECT-only traffic's session teardown path.
  ${ALIRO_UD_APP_SRC_DIR}/storage/mailbox/mailbox.cpp
  ${ALIRO_UD_APP_SRC_DIR}/storage/mailbox/mailbox_sessions.cpp
  ${ALIRO_UD_APP_SRC_DIR}/storage/mailbox/mailbox_store.cpp
  ${ALIRO_UD_TEST_COMMON_DIR}/fake_mailbox_persistence.cpp
  ${ALIRO_UD_WORKER_LIFECYCLE_TEST_SRC_DIR}/fake_nfc_interface.cpp
  src/test_authorization_window.cpp
  src/test_authorization_contract.cpp
  src/test_authorization_e2e.cpp
)

# nfc_worker.cpp calls into BeginCommandTiming()/EndCommandTiming()
# (APP_PLAN.md AWP7); mirrors the application's own CMakeLists.txt (see
# worker_lifecycle's copy of this comment for the full rationale).
target_sources_ifdef(CONFIG_ALIRO_UD_TIMING_INSTRUMENTATION app PRIVATE
  ${ALIRO_UD_APP_SRC_DIR}/platform/nfc/command_timing.cpp
)
