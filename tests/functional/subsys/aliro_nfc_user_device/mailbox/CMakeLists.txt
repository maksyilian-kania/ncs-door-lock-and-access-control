#
# Copyright (c) 2026 Nordic Semiconductor ASA
#
# SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
#

cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})

project(aliro_nfc_user_device_mailbox)

set(ALIRO_UD_APP_SRC_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../../../../applications/aliro-nfc-user-device/src)
set(ALIRO_UD_TEST_COMMON_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../common)

target_include_directories(app PRIVATE ${ALIRO_UD_APP_SRC_DIR} ${ALIRO_UD_TEST_COMMON_DIR})

# Aliro::Interface::UserDevice::Mailbox implementation under test
# (APP_PLAN.md AWP6): the real mailbox.cpp/mailbox_sessions.cpp/
# mailbox_store.cpp engine, plus the real AWP3 credential store (mailbox
# configuration lives there) with host-test fakes standing in for the
# hardware-backed credential/mailbox persistence and key backends, exactly
# as the other functional tests.
target_sources(app PRIVATE
  ${ALIRO_UD_APP_SRC_DIR}/storage/mailbox/mailbox.cpp
  ${ALIRO_UD_APP_SRC_DIR}/storage/mailbox/mailbox_sessions.cpp
  ${ALIRO_UD_APP_SRC_DIR}/storage/mailbox/mailbox_store.cpp
  ${ALIRO_UD_TEST_COMMON_DIR}/fake_mailbox_persistence.cpp
  ${ALIRO_UD_APP_SRC_DIR}/storage/credential/credential_store.cpp
  ${ALIRO_UD_TEST_COMMON_DIR}/fake_credential_persistence.cpp
  ${ALIRO_UD_TEST_COMMON_DIR}/fake_key_backend.cpp
  src/test_mailbox_store.cpp
  src/test_mailbox_sessions.cpp
  src/test_mailbox_fault_injection.cpp
)
