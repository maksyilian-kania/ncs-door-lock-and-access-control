#
# Copyright (c) 2026 Nordic Semiconductor ASA
#
# SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
#

cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})

project(aliro_nfc_user_device_apdu_fragment_assembler)

set(ALIRO_UD_APP_SRC_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../../../../applications/aliro-nfc-user-device/src)

# The Aliro headers used by apdu_fragment_assembler.h (etl::span) come from
# the "etl" west module, normally pulled in transitively by the
# NCS_ALIRO/NCS_ALIRO_USER_DEVICE Kconfig option; this test has neither, so
# its include path is added explicitly.
target_include_directories(app PRIVATE
  ${ALIRO_UD_APP_SRC_DIR}
  ${ZEPHYR_BASE}/../modules/lib/etl/include
)

target_sources(app PRIVATE
  ${ALIRO_UD_APP_SRC_DIR}/platform/nfc/apdu_fragment_assembler.cpp
  src/test_apdu_fragment_assembler.cpp
)
