# System OFF support — design discussion (proposal, not yet implemented)

Status: implemented in `src/platform/power/` as a standalone feature, not
an Application Work Package (no `AWPx` label; not committed until reviewed
and approved). `APP_PLAN.md` no longer excludes System OFF (the exclusion
was removed as part of this proposal); AWP0's historical record of removing
the old PoC's System OFF code is unchanged. See `docs/traceability.md`'s
"System OFF (post-AWP8 feature, not an AWP)" note for the implementation
summary and remaining verification gaps.

## Wake sources

- **NFC field detection** — existing mechanism (NFCT low-power field detect),
  unaffected by this proposal.
- **Button 1** (`sw1`) — press wakes the DK; press again while awake puts it
  back to sleep. Needs dual pin-mode: level-sense (`SENSE`) configured right
  before `sys_poweroff()`, edge-interrupt while running.
- **Button 0 is explicitly NOT a wake source.** It keeps its current, sole
  role (opening the 30 s authorization window via
  `authorization_button.cpp`), unmodified. Dropping it from wake duty avoids
  needing to attribute a wake reset to a specific GPIO pin.


## Sleep policy

- Auto-sleep on NFC field-off after a **5 s** idle delay (mirrors
  `samples/nfc/system_off`'s pattern: reschedule/cancel a `k_work_delayable`
  on `FIELD_ON`/`FIELD_OFF`, longer than the sample's 3 s).
- The 30 s button-authorization window (`authorization_window.cpp`) must
  never be interrupted by an *automatic* (idle-timeout) power-off. This
  needs new coordination: `Window` currently has no expiry callback (poll
  only), so an arbiter combining {NFC field state, window validity} must be
  added before the idle-timeout path is allowed to call `sys_poweroff()`.
- **Button 1's explicit "sleep now" press is NOT gated by the window or by
  an active NFC session.** It can power off the DK at any time, including
  mid-transaction or mid-30s-window. This is a deliberate PoC simplification
  (operator is trusted to know what they're doing).
- Suspend the console device (`pm_device_action_run(cons,
  PM_DEVICE_ACTION_SUSPEND)`) before every `sys_poweroff()` call, so no
  shell/log output is truncated (matches both reference samples). Requires
  `CONFIG_PM_DEVICE=y` (and likely `CONFIG_PM_DEVICE_RUNTIME=y`).

## LED indication

- `led0` stays exactly as-is: the existing "authorization required, no
  valid window" indicator (`authorization_led.cpp`, `authorization.cpp`).
  **Do not repurpose it.**
- `led1` becomes the sole "DK is awake / CLI is reachable" indicator: on at
  boot (any wake source), off immediately before `sys_poweroff()`. No need
  to distinguish wake source for this — "awake" is true whenever the shell
  is reachable, whatever woke it.

## Build-time disable and layout

- New Kconfig symbol `CONFIG_ALIRO_UD_SYSTEM_OFF` (default `y`).
- New module `src/platform/power/` (own `CMakeLists.txt`/`Kconfig`), source
  file(s) added via `target_sources_ifdef(CONFIG_ALIRO_UD_SYSTEM_OFF ...)`,
  following the existing `platform/nfc`/`CONFIG_ALIRO_UD_TIMING_INSTRUMENTATION`
  pattern. Every call site elsewhere is wrapped in `IS_ENABLED(...)`, so the
  whole feature can be disabled from `prj.conf` alone with no code changes.
  This does **not** come for free from `CONFIG_POWEROFF` alone — that only
  makes `sys_poweroff()` available; the scheduling/call-site logic must be
  deliberately gated.
- No new `boards/` overlay: `sw1` and `led1` devicetree aliases already
  exist in the stock `nrf54lm20dk_common.dtsi`; only `prj.conf` additions
  are needed (`CONFIG_POWEROFF`, `CONFIG_PM_DEVICE`, the new Kconfig symbol).

## Caveats / open risks

1. **GPIO wake-source attribution is unreliable on this chip family** — not
   needed anymore since Button 0 was dropped from the wake path, but
   documented here because it's why that decision was made. Nordic DevZone
   reports both LATCH bits set for two SENSE-configured pins even though
   only one was pressed (on nRF54LS05B, `SENSE_LOW` config): see
   <https://devzone.nordicsemi.com/f/nordic-q-a/128056/latch-register-does-not-correctly-indicate-gpio-after-wakeup-from-system-off>.
   GPIO pin state can also be unstable for up to ~800 ms after a wake reset:
   <https://devzone.nordicsemi.com/f/nordic-q-a/109003/trouble-with-gpios-in-system-off-mode>.
   Not yet confirmed against the nRF54LM20's own errata; moot for the
   current (Button-1-only, no attribution needed) design, but relevant if
   scope changes again.
2. **Button 1 debounce**: arming its "sleep on next press" edge interrupt
   too early after boot risks a spurious re-sleep from the same press's
   release-bounce that woke the board. Resolved: a
   `CONFIG_ALIRO_UD_SYSTEM_OFF_BUTTON1_SETTLE_MS` (default 300 ms) delay
   before arming (`src/platform/power/power_button1.cpp`).
3. **Power-off during a persistent CLI mutation** (`credential
   commit/delete/reset`, `mailbox init/reset`): resolved with an explicit
   gate, not just reliance on the existing four-phase crash-safe journal
   (NVS + PSA/CRACEN/KMU) being poweroff-safe. `AliroUd::Lifecycle::RunMutation()`
   now brackets the mutation with `AliroUd::Power::BeginMutationGuard()`/
   `EndMutationGuard()`: any power-off trigger (Button 1 or the auto
   idle-timeout) that becomes due while a mutation is in flight is
   deferred, not dropped, and fires the instant the mutation ends. This
   removes the *need* to rely on poweroff-safety of the journal for
   correctness, but does not replace verifying it: the project's own
   traceability entry (`ALIRO-UD-SYRS-P1-007`) still notes general
   power-loss fault injection beyond the mailbox portion as a host-test
   gap, and a deliberate on-target fault-injection pass (repeatedly power
   off mid-`commit`, exercising the gate itself and any residual race) is
   still recommended before treating this as fully verified.
4. **Live NFC session interrupted by power-off**: not a real risk — all
   session state is RAM-only and reinitializes cleanly on next boot,
   equivalent to the already-anticipated "field pulled away mid-transaction"
   case documented in `docs/provisioning.md`.
5. **30 s window lost on Button-1 sleep**: accepted UX tradeoff, not a
   correctness issue (window state is RAM-only); operator must re-press
   Button 0 after the next wake.

## Reminder

Once implemented, explicitly test with System OFF **enabled** on real DK
hardware (NFC-field wake, Button‑1 wake/sleep, auto-sleep after the 5 s idle
delay, and the mid-`commit` fault-injection pass above) — do not rely on
host/`native_sim` testing alone, since none of the wake/power hardware paths
exist there.
