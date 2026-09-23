#
# Copyright (c) 2026 Nordic Semiconductor ASA
#
# SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
#

cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})

project(aliro_nfc_user_device_power)

set(ALIRO_UD_APP_SRC_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../../../../applications/aliro-nfc-user-device/src)

target_include_directories(app PRIVATE ${ALIRO_UD_APP_SRC_DIR})

# power_policy.cpp (pure decision logic, docs/system_off_proposal.md) only:
# power.cpp/power_button1.cpp/power_indicator.cpp are DK-hardware-only
# (real GPIO, sys_poweroff()) and excluded here, the same split used for
# authorization_button.cpp/authorization_led.cpp in the authorization test.
target_sources(app PRIVATE
  ${ALIRO_UD_APP_SRC_DIR}/platform/power/power_policy.cpp
  src/test_power_policy.cpp
)
