#
# Copyright (c) 2026 Nordic Semiconductor ASA
#
# SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
#

cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})

project(aliro_nfc_user_device_persistent_key)

set(ALIRO_UD_APP_SRC_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../../../../applications/aliro-nfc-user-device/src)

target_include_directories(app PRIVATE ${ALIRO_UD_APP_SRC_DIR} src)

# Aliro::Interface::UserDevice::PersistentKey adapter and record engine under
# test, independent of protocol flow. The persistence and PSA key backends are
# suite-local in-memory fakes with fault injection and object accounting;
# neither crypto.cpp nor the credential store is linked.
target_sources(app PRIVATE
  ${ALIRO_UD_APP_SRC_DIR}/storage/key/persistent_key.cpp
  ${ALIRO_UD_APP_SRC_DIR}/storage/key/persistent_key_store.cpp
  src/fake_psa_key_backend.cpp
  src/fake_record_persistence.cpp
  src/test_persistent_key_semantics.cpp
)
