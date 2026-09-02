#
# Copyright (c) 2026 Nordic Semiconductor ASA
#
# SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
#

zephyr_include_directories(include)
zephyr_include_directories(nordic-axon-model)

zephyr_library()

zephyr_library_sources(
	src/gesture_access.cpp
	src/gesture_access_model.cpp
)

zephyr_library_sources_ifdef(CONFIG_DOOR_LOCK_GESTURE_ACCESS_FRAME_FORWARDING src/frame_forwarding.cpp)
