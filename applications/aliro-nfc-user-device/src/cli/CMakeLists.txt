#
# Copyright (c) 2026 Nordic Semiconductor ASA
#
# SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
#

# Development CLI over the DK virtual UART (Zephyr shell, "aliro-ud" root
# command). Implemented starting with AWP2.
target_sources(app PRIVATE cli.cpp)
