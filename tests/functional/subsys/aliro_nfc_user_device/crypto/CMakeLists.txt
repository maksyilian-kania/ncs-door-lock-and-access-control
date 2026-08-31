#
# Copyright (c) 2026 Nordic Semiconductor ASA
#
# SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
#

cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})

project(aliro_nfc_user_device_crypto)

set(ALIRO_UD_APP_SRC_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../../../../applications/aliro-nfc-user-device/src)
set(ALIRO_UD_TEST_COMMON_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../common)

target_include_directories(app PRIVATE ${ALIRO_UD_APP_SRC_DIR} ${ALIRO_UD_TEST_COMMON_DIR})

# Aliro::Interface::UserDevice::Crypto/CredentialSigning implementation under
# test (APP_PLAN.md AWP5), plus the real AWP3 credential store/key backend
# (with host-test fakes for the persistence/key backends, exactly as the
# other functional tests) so test_credential_signing.cpp can exercise
# CredentialSigning::Sign() against a real provisioned credential.
target_sources(app PRIVATE
  ${ALIRO_UD_APP_SRC_DIR}/platform/crypto/crypto.cpp
  ${ALIRO_UD_APP_SRC_DIR}/platform/crypto/certificate.cpp
  ${ALIRO_UD_APP_SRC_DIR}/platform/crypto/credential_signing.cpp
  ${ALIRO_UD_APP_SRC_DIR}/storage/credential/credential_store.cpp
  ${ALIRO_UD_TEST_COMMON_DIR}/fake_credential_persistence.cpp
  ${ALIRO_UD_TEST_COMMON_DIR}/fake_key_backend.cpp
  src/test_crypto.cpp
  src/test_certificate.cpp
  src/test_credential_signing.cpp
)
