#
# Copyright (c) 2026 Nordic Semiconductor ASA
#
# SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
#

# Aliro::Interface::UserDevice::Credential and Trust contracts: Zephyr
# settings/NVS-backed non-secret metadata, per-binding reader trust, and the
# persistent transaction journal spanning NVS and PSA key storage.
# Implemented in AWP3.
#
# credential.cpp is a placeholder (empty credential set, "not provisioned"
# trust) added only to keep the application linking against the currently
# checked-out stack (ncs-aliro WP5); AWP3 replaces its contents.
target_sources(app PRIVATE credential.cpp)
