#
# Copyright (c) 2026 Nordic Semiconductor ASA
#
# SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
#

cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})

project(aliro_nfc_user_device_worker_lifecycle)

set(ALIRO_UD_APP_SRC_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../../../../applications/aliro-nfc-user-device/src)
set(ALIRO_UD_TEST_COMMON_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../common)

target_include_directories(app PRIVATE ${ALIRO_UD_APP_SRC_DIR} ${ALIRO_UD_TEST_COMMON_DIR})

# Real platform/nfc worker (bounded queue + dedicated thread) and platform/os
# (mutex, timer, deferred-event queue, logging) code under test, built
# exactly as shipped. nfc_transport.cpp (the nfc_t4t_lib hardware glue) is
# intentionally excluded: this test substitutes fake_nfc_interface.cpp for
# Aliro::Interface::UserDevice::Nfc so it can run on native_sim/native/64
# without real NFC hardware, while still exercising the real worker
# thread/queue and the real Aliro::UserDeviceStack (AWP1).
target_sources(app PRIVATE
  ${ALIRO_UD_APP_SRC_DIR}/platform/nfc/nfc_worker.cpp
  ${ALIRO_UD_APP_SRC_DIR}/platform/os/os_mutex.cpp
  ${ALIRO_UD_APP_SRC_DIR}/platform/os/os_timer.cpp
  ${ALIRO_UD_APP_SRC_DIR}/platform/os/os_queue.cpp
  ${ALIRO_UD_APP_SRC_DIR}/platform/os/os_trusted_time.cpp
  ${ALIRO_UD_APP_SRC_DIR}/platform/os/os_logging.cpp
  ${ALIRO_UD_APP_SRC_DIR}/platform/os/transaction.cpp
  # Aliro::Interface::UserDevice::Credential/Trust adapter (AWP3) and its
  # real credential_store.cpp engine; the real Aliro::UserDeviceStack pulled
  # in below calls into these unconditionally (ncs-aliro WP5's
  # CredentialManager::EvaluateAuth0()). credential_persistence_settings.cpp
  # and key_backend_psa.cpp are swapped for in-memory fakes (native_sim has
  # no hardware-backed flash/trusted storage); see
  # tests/functional/subsys/aliro_nfc_user_device/common/.
  ${ALIRO_UD_APP_SRC_DIR}/storage/credential/credential.cpp
  ${ALIRO_UD_APP_SRC_DIR}/storage/credential/credential_store.cpp
  ${ALIRO_UD_TEST_COMMON_DIR}/fake_credential_persistence.cpp
  ${ALIRO_UD_TEST_COMMON_DIR}/fake_key_backend.cpp
  # Aliro::Interface::UserDevice::Authorization adapter (AWP4) and its real
  # authorization_window.cpp state machine; authorization_button.cpp/
  # authorization_led.cpp (DK-hardware-only GPIO) are excluded, substituted
  # by fake_authorization_indicator.cpp.
  ${ALIRO_UD_APP_SRC_DIR}/platform/authorization/authorization.cpp
  ${ALIRO_UD_APP_SRC_DIR}/platform/authorization/authorization_window.cpp
  ${ALIRO_UD_TEST_COMMON_DIR}/fake_authorization_indicator.cpp
  src/fake_nfc_interface.cpp
  src/test_worker_lifecycle.cpp
)
