#
# Copyright (c) 2026 Nordic Semiconductor ASA
#
# SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
#

# Aliro::Interface::UserDevice::Authorization contract backed by a DK button
# and visible LED indication. Implemented in AWP4.
#
# authorization.cpp is a placeholder (fails closed: always "Required", never
# granted) added only to keep the application linking against the currently
# checked-out stack (ncs-aliro WP5); AWP4 replaces its contents.
target_sources(app PRIVATE authorization.cpp)
