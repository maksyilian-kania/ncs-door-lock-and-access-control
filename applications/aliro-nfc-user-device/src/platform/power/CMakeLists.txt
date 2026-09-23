#
# Copyright (c) 2026 Nordic Semiconductor ASA
#
# SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
#

# System OFF support (docs/system_off_proposal.md): NFC-field/Button-0
# wake, Button-1 hold-awake/off toggle, auto-idle sleep. Entirely removable
# from production builds: no source file here is even compiled in when
# CONFIG_ALIRO_UD_SYSTEM_OFF is disabled.
target_sources_ifdef(CONFIG_ALIRO_UD_SYSTEM_OFF app PRIVATE
  power_policy.cpp
  power.cpp
  power_button0.cpp
  power_button1.cpp
  power_indicator.cpp
)
