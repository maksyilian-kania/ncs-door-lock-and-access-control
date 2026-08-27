#
# Copyright (c) 2026 Nordic Semiconductor ASA
#
# SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
#

cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(aliro-nfc-user-device)

target_sources(app PRIVATE src/main.cpp)

add_subdirectory(src/platform/nfc)
add_subdirectory(src/platform/os)
add_subdirectory(src/platform/crypto)
add_subdirectory(src/platform/authorization)
add_subdirectory(src/storage/credential)
add_subdirectory(src/storage/mailbox)
add_subdirectory(src/cli)
