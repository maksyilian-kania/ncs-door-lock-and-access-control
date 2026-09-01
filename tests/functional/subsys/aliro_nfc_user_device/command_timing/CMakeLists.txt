#
# Copyright (c) 2026 Nordic Semiconductor ASA
#
# SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
#

cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})

project(aliro_nfc_user_device_command_timing)

set(ALIRO_UD_APP_SRC_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../../../../applications/aliro-nfc-user-device/src)
set(ALIRO_UD_TEST_COMMON_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../common)

target_include_directories(app PRIVATE ${ALIRO_UD_APP_SRC_DIR} ${ALIRO_UD_TEST_COMMON_DIR})

# Real platform/nfc worker (bounded queue + dedicated thread) and platform/os
# code under test, built exactly as shipped (see worker_lifecycle's
# CMakeLists.txt for the full rationale). nfc_transport.cpp is intentionally
# excluded in favor of the shared fake_nfc_interface.cpp so this test can run
# on native_sim/native/64 (APP_PLAN.md AWP7: command-to-response timing
# instrumentation).
target_sources(app PRIVATE
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
  ${ALIRO_UD_APP_SRC_DIR}/platform/authorization/authorization.cpp
  ${ALIRO_UD_APP_SRC_DIR}/platform/authorization/authorization_window.cpp
  ${ALIRO_UD_TEST_COMMON_DIR}/fake_authorization_indicator.cpp
  src/fake_nfc_interface.cpp
  src/test_command_timing.cpp
)

# Mirrors the application's own CMakeLists.txt: the CommandTiming module is
# only compiled in when the Kconfig option is enabled, so the "disabled"
# testcase.yaml scenario (extra_configs:
# CONFIG_ALIRO_UD_TIMING_INSTRUMENTATION=n) exercises the exact same
# no-op/removed-from-the-build behavior a production build would have.
target_sources_ifdef(CONFIG_ALIRO_UD_TIMING_INSTRUMENTATION app PRIVATE
  ${ALIRO_UD_APP_SRC_DIR}/platform/nfc/command_timing.cpp
)
