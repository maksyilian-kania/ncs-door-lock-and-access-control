#
# Copyright (c) 2026 Nordic Semiconductor ASA
#
# SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
#

# Aliro::Interface::UserDevice::Os contract (event queue, mutex, timer, and
# trusted-time bindings) implemented with Zephyr primitives (AWP1).
target_sources(app PRIVATE
  os_mutex.cpp
  os_timer.cpp
  os_queue.cpp
  os_trusted_time.cpp
  os_logging.cpp
  app_status.cpp
  # Aliro::Interface::UserDevice::Transaction contract (WP5.5 decision D8);
  # a log-only stub is sufficient until a later AWP requires real behavior.
  transaction.cpp
)
