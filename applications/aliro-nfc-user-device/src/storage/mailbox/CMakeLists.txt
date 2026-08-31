#
# Copyright (c) 2026 Nordic Semiconductor ASA
#
# SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
#

# Aliro::Interface::UserDevice::Mailbox contract: per-credential persistent
# mailbox with snapshot, staged mutation, atomic commit, rollback, and close
# semantics (APP_PLAN.md AWP6).
#
# mailbox.cpp is a thin adapter to mailbox_sessions.cpp (the Reader-facing
# session engine), which in turn calls mailbox_store.cpp (the
# Credential-Issuer-level committed-byte storage engine, used directly by
# the CLI too). mailbox_persistence_settings.cpp is the real
# (Zephyr-settings-backed) persistence backend; host tests swap in an
# application-owned in-memory fake instead (see
# tests/functional/subsys/aliro_nfc_user_device/common/).
target_sources(app PRIVATE
  mailbox.cpp
  mailbox_sessions.cpp
  mailbox_store.cpp
  mailbox_persistence_settings.cpp
)
