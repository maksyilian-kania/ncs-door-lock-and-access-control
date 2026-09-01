#
# Copyright (c) 2026 Nordic Semiconductor ASA
#
# SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
#

# NFC-A Type 4 Tag/ISO-DEP listen transport: connects nfc_t4t_lib to the
# Aliro::UserDeviceStack session/command-APDU facade through a bounded queue
# and dedicated NFC/stack thread (AWP1).
target_sources(app PRIVATE
  apdu_fragment_assembler.cpp
  nfc_worker.cpp
  nfc_transport.cpp
)

# Command-to-response timing instrumentation (APP_PLAN.md AWP7): entirely
# removable from production builds, so its one source file is only added to
# the build at all when CONFIG_ALIRO_UD_TIMING_INSTRUMENTATION is enabled.
target_sources_ifdef(CONFIG_ALIRO_UD_TIMING_INSTRUMENTATION app PRIVATE command_timing.cpp)
