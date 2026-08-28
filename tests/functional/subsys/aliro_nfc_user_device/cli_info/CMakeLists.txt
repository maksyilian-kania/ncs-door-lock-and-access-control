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

target_include_directories(app PRIVATE ${ALIRO_UD_APP_SRC_DIR})

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
  # Aliro::Interface::UserDevice::Credential/Trust/Authorization placeholder
  # stubs (AWP3/AWP4 own the real implementations); see
  # applications/aliro-nfc-user-device/src/storage/credential/credential.cpp.
  ${ALIRO_UD_APP_SRC_DIR}/storage/credential/credential.cpp
  ${ALIRO_UD_APP_SRC_DIR}/platform/authorization/authorization.cpp
  ${ALIRO_UD_WORKER_LIFECYCLE_TEST_SRC_DIR}/fake_nfc_interface.cpp
  src/test_cli_info.cpp
)
