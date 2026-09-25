#
# Copyright (c) 2026 Nordic Semiconductor ASA
#
# SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
#

cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})

project(aliro_nfc_user_device_os_timer)

set(ALIRO_UD_APP_SRC_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../../../../applications/aliro-nfc-user-device/src)

# The real Aliro::Interface::UserDevice::Os::Timer adapter in isolation.
# os_mutex.cpp registers the aliro_ud_os log module the adapter logs to.
target_sources(app PRIVATE
  ${ALIRO_UD_APP_SRC_DIR}/platform/os/os_mutex.cpp
  ${ALIRO_UD_APP_SRC_DIR}/platform/os/os_timer.cpp
  src/test_os_timer.cpp
)
