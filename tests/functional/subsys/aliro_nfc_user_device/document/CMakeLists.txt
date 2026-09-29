#
# Copyright (c) 2026 Nordic Semiconductor ASA
#
# SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
#

cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})

project(aliro_nfc_user_device_document)

set(ALIRO_UD_APP_SRC_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../../../../applications/aliro-nfc-user-device/src)
set(ALIRO_UD_TEST_COMMON_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../common)

target_include_directories(app PRIVATE ${ALIRO_UD_APP_SRC_DIR} ${ALIRO_UD_TEST_COMMON_DIR})

# Aliro::Interface::UserDevice::Document adapter and snapshot engine under
# test, over the real credential store (the single source of provisioned
# document bytes) with the shared in-memory persistence and key-backend
# fakes. No stack protocol code is linked.
target_sources(app PRIVATE
  ${ALIRO_UD_APP_SRC_DIR}/storage/document/document.cpp
  ${ALIRO_UD_APP_SRC_DIR}/storage/document/document_snapshots.cpp
  ${ALIRO_UD_APP_SRC_DIR}/storage/credential/credential_store.cpp
  ${ALIRO_UD_TEST_COMMON_DIR}/fake_credential_persistence.cpp
  ${ALIRO_UD_TEST_COMMON_DIR}/fake_key_backend.cpp
  # credential_store.cpp cascades credential deletion to the Kpersistent
  # record store, whose key backend fake resolves keys through crypto.cpp.
  ${ALIRO_UD_APP_SRC_DIR}/storage/key/persistent_key_store.cpp
  ${ALIRO_UD_TEST_COMMON_DIR}/fake_persistent_key_persistence.cpp
  ${ALIRO_UD_TEST_COMMON_DIR}/fake_persistent_key_backend.cpp
  ${ALIRO_UD_APP_SRC_DIR}/platform/crypto/crypto.cpp
  ${ALIRO_UD_APP_SRC_DIR}/platform/crypto/certificate.cpp
  src/test_document_snapshots.cpp
)
