#
# Copyright (c) 2026 Nordic Semiconductor ASA
#
# SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
#

# Aliro::Interface::UserDevice::Credential and Trust contracts (APP_PLAN.md
# AWP3): Zephyr settings/NVS-or-ZMS-backed non-secret metadata, per-binding
# reader trust, PSA/CRACEN/KMU-backed persistent private-key storage, and
# the persistent transaction journal spanning both.
#
# credential.cpp is a thin adapter to credential_store.cpp, the real
# engine. credential_persistence_settings.cpp and key_backend_psa.cpp are
# the real (hardware-backed) persistence/key backends; host tests swap in
# application-owned in-memory fakes instead (see
# tests/functional/subsys/aliro_nfc_user_device/common/).
target_sources(app PRIVATE
  credential.cpp
  credential_store.cpp
  credential_persistence_settings.cpp
  key_backend_psa.cpp
)
