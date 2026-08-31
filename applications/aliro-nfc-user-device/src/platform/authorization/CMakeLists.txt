#
# Copyright (c) 2026 Nordic Semiconductor ASA
#
# SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
#

# Aliro::Interface::UserDevice::Authorization contract backed by a DK button
# and visible LED indication (APP_PLAN.md AWP4). authorization_window.cpp is
# the pure, host-testable window state machine; authorization.cpp adapts it
# to the public contract; authorization_button.cpp/authorization_led.cpp are
# the DK-hardware-only GPIO backends (excluded from host tests, which link
# tests/.../common/fake_authorization_indicator.cpp instead).
target_sources(app PRIVATE
  authorization_window.cpp
  authorization.cpp
  authorization_button.cpp
  authorization_led.cpp
)
