# AWP4 — Local authorization and visible indication

Part of the per-AWP evidence log (`docs/evidence/`); see `docs/STATE.md` for
the current checkpoint and `../traceability.md` for requirement status.
Previous: `AWP3.md`. Next: `AWP5.md`.

## Baseline

- West topdir: `/home/mak5-local/gesture-access` (manifest file
  `west-aliro.yml`).
- `ncs-aliro` checked-out revision:
  `fa606452ab587bf7aaae85506962f1e340545471` ("user_device: complete WP5.5
  remediation"), confirmed a descendant of the minimum baseline
  `b8bed857b482d288168185e76d5452469739fbdd`
  (`git -C <west-topdir>/ncs-aliro merge-base --is-ancestor b8bed857... HEAD`).
  No `west update` was run.

## Public contract inspected

- `include/aliro/user_device/interface.h` —
  `Aliro::Interface::UserDevice::Authorization::GetState(CredentialHandle,
  AuthorizationState&)` (WP5.5, decision D7: error-bearing; `outState` must
  be initialized to `Denied` and left there only on error) and
  `::NotifyAuthenticationRequired(CredentialHandle)`.
- `include/aliro/user_device/types.h` — `enum class AuthorizationState {
  Authorized, Denied, Required }`.
- `stack/src/user_device/credential_manager.cpp` —
  `CredentialManager::EvaluateAuth0()`: combines the Reader-requested
  `authentication_policy` (AUTH0 Table 8-1) with the resolved credential's
  own provisioned policy (`credentialForcesAuthentication`); either one
  being `ForceUserAuthentication` (0x03) triggers exactly one
  `Authorization::GetState()` call, whose result gates the outcome
  (`kProceed`/`kAuthenticationRequired`/`kDenied`). Neither 0x01
  (`UserDeviceSetting`) nor 0x02 (`UserDeviceSettingSecureAction`) alone
  forces the check — only a credential provisioned with
  `AuthenticationPolicy::ForceUserAuthentication` does, or a Reader request
  of 0x03. `GetState()` receives only a `CredentialHandle`, never the
  Reader-requested policy value, so the application-owned backend cannot
  (and does not need to) distinguish 0x01/0x02/0x03 itself: it implements
  one uniform button-window gate, and the stack's already-tested
  `EvaluateAuth0()` combining logic is what makes "0x01/0x02/0x03 ... require
  a valid button authorization window" true whenever either policy forces
  it (`APP_PLAN.md` AWP4 deliverable wording; see `stack/tests/user_device/credential/src/test_policy.cpp`
  for the stack's own coverage of this combination).
- `stack/src/user_device/hsm/state_machine.cpp` —
  `UserDeviceStateMachine::HandleAuth0()`: on
  `Auth0Outcome::kAuthenticationRequired`, posts a deferred
  `SessionLifecycleEvent::AuthenticationRequired` through `EventHandler`
  (never calls `NotifyAuthenticationRequired()` synchronously from AUTH0
  processing); the AUTH0 response is `StatusWord::kFunctionNotSupported`
  (empty data) for every outcome (`kProceed`/`kAuthenticationRequired`/
  `kDenied`/`kNoMatch`) alike, since WP6 AUTH0 cryptography does not exist
  yet (`ALIRO-UD-SYRS-P1-031`).

## Deliverables completed

- `src/platform/authorization/authorization_window.{h,cpp}` — the pure,
  host-testable `AliroUd::Authorization::Window` state machine: one
  device-global window (`GlobalWindow()`) shared by every credential handle
  (the public contract's `GetState()` carries only a `CredentialHandle`, and
  a physical button press represents device-operator presence independent
  of which credential a Reader selects). Every method takes an explicit
  `nowMs` parameter rather than calling `k_uptime_get()` internally, so the
  window logic itself has no clock dependency (`APP_PLAN.md` AWP4 Verify:
  "with a fake monotonic clock"). Guarded by a `k_spinlock` (ISR-safe, for
  the real DK button callback).
- `src/platform/authorization/authorization.cpp` — the real
  `Aliro::Interface::UserDevice::Authorization` adapter: `GetState()` reads
  `GlobalWindow().GetState(k_uptime_get())` and returns synchronously (never
  waits inside an NFC transaction for a button press, per `APP_PLAN.md`
  AWP4: "Return the synchronous Required state so the stack fails
  promptly"); `NotifyAuthenticationRequired()` turns the visible indication
  on via `Indicator::SetActive(true)`.
- `src/platform/authorization/authorization_indicator.h` +
  `authorization_led.cpp` — the visible-indication interface and its real
  DK LED backend (`led0` devicetree alias, active-high GPIO output),
  configured lazily on first use.
- `src/platform/authorization/authorization_button.cpp` — the real DK
  button backend: a `sw0`-alias GPIO interrupt (`GPIO_INT_EDGE_TO_ACTIVE`)
  opens `GlobalWindow()` for
  `CONFIG_ALIRO_UD_AUTHORIZATION_WINDOW_SECONDS` seconds and clears the
  visible indication. `#if DT_HAS_ALIAS(...)`-guarded, matching the
  `aliro-access-control-app` DFU-button convention; both DK board variants
  in this repository (`nrf54lm20dk_nrf54lm20{a,b}_cpuapp`) define `sw0`/
  `led0` in their common board `.dtsi`, so no application board overlay was
  needed.
- `src/platform/authorization/Kconfig` — new
  `CONFIG_ALIRO_UD_AUTHORIZATION_WINDOW_SECONDS` (range 1-300, default 30,
  per `APP_PLAN.md` AWP4) plus the standard per-module `LOG_LEVEL` choice;
  `rsource`d from `src/platform/Kconfig`.
- `src/cli/cli.cpp` — `aliro-ud auth status` (reports
  `state=authorized|required` and `remaining_ms=`), `aliro-ud auth press`
  (test trigger: opens the window and clears the indication exactly like a
  real button press — `APP_PLAN.md` AWP4 Verify: "Provide an application
  test trigger if the current stack cannot request authorization end to
  end"), `aliro-ud auth clear` (test trigger: immediately revokes the
  window), and `aliro-ud auth notify-required` (test trigger, added during
  the DK demonstration below because no NFC reader was available in this
  environment to drive a real AUTH0 exchange end to end: calls
  `Aliro::Interface::UserDevice::Authorization::NotifyAuthenticationRequired()`
  directly, the same call the real stack makes for an AUTH0 policy 0x01-0x03
  credential with no valid window — same "test trigger" allowance). All
  four are deterministic, secret-free `OK`/`ERR` lines following the AWP2
  CLI contract.
- `src/platform/authorization/authorization_button.cpp` — fixed during the
  DK demonstration below: the GPIO ISR callback now only `k_work_submit()`s;
  `Window::Open()`, `Indicator::SetActive()` (including its lazy first-call
  `gpio_pin_configure_dt()`), and the `LOG_INF()` under
  `CONFIG_LOG_MODE_IMMEDIATE=y` all now run on the system workqueue thread
  instead of GPIO ISR context.
- `prj.conf` — `CONFIG_MAIN_STACK_SIZE` raised from 4096 to 8192; see the
  on-target crash this fixes under "Commands run and results" below.
- `tests/functional/subsys/aliro_nfc_user_device/authorization/` — new host
  test target (17 cases):
  - `test_authorization_window.cpp` (11 cases, pure logic, fake
    monotonic clock via explicit `nowMs`): never-opened state, opening
    before a later query ("preauthorization"), exact 1-second and
    300-second Kconfig-range boundaries, expiry exactly at the deadline, a
    repeated press before expiry extending the window ("retry"/"repeated
    presses"), a repeated press after expiry reopening a fresh window,
    `Close()` immediately revoking an open window ("denial"/explicit
    reset), `GetRemainingMs()` countdown, and `GlobalWindow()` singleton
    identity.
  - `test_authorization_contract.cpp` (4 cases): the real
    `Aliro::Interface::UserDevice::Authorization::GetState()`/
    `NotifyAuthenticationRequired()` functions against the real window and
    a new `tests/.../common/fake_authorization_indicator.{h,cpp}` recorder
    (substituting for the DK-hardware-only `authorization_led.cpp`, the
    same split used for `nfc_transport.cpp`/`fake_nfc_interface.cpp|`):
    `Required` with no window, `Authorized` after `GlobalWindow().Open()`,
    credential-handle-independence, and exactly one indicator activation
    per `NotifyAuthenticationRequired()` call.
  - `test_authorization_e2e.cpp` (3 cases): a **real** AUTH0 command
    (`authentication_policy` 0x03) sent through the real bounded
    queue/worker thread and real `Aliro::UserDeviceStack`, against a real
    credential provisioned through this application's own
    `credential_store::Create()` with
    `AuthenticationPolicy::ForceUserAuthentication` — with no window open,
    exactly one deferred `NotifyAuthenticationRequired()`/indicator
    activation; with the window opened first (`GlobalWindow().Open()`,
    standing in for a physical press), zero activations; and the AUTH0
    response bytes are asserted byte-identical in both cases
    (`ALIRO-UD-SYRS-P1-031`).
  - `worker_lifecycle`/`cli_info` `CMakeLists.txt` updated to link the real
    `authorization_window.cpp` and the new fake indicator (previously they
    linked only the AWP2/AWP3-stub `authorization.cpp`).
  - `cli_info`'s `test_auth_status_press_clear` covers the new CLI commands
    through the dummy shell backend.

## Commands run and results

Toolchain invoked via `ncs4`, from `/home/mak5-local/gesture-access` (west
topdir).

1. `west twister -T ncs-door-lock-and-access-control.git/tests/functional/subsys/aliro_nfc_user_device -p native_sim/native/64`
   — **Result: pass.** 5 of 5 test configurations, 42/42 test cases
   (`host_smoke`, `apdu_fragment_assembler`, `worker_lifecycle` [12],
   `authorization` [17 — new], `cli_info` [5, including the new
   `test_auth_status_press_clear`]) — reconfirmed pass after every fix
   below (button-ISR deferral, `CONFIG_MAIN_STACK_SIZE`, the
   `notify-required` CLI trigger).
2. `west build -b nrf54lm20dk/nrf54lm20b/cpuapp -d build_awp4_dk ncs-door-lock-and-access-control.git/applications/aliro-nfc-user-device --pristine`
   — **Result: pass.** Final FLASH 149900 B (7.19%), RAM 82120 B (15.69%)
   (up from AWP3's build; besides the real GPIO button/LED backends and
   PSA/NVS from AWP3, the increase over this AWP's first pass is the
   doubled `CONFIG_MAIN_STACK_SIZE`, below).
3. Physical DK flash-and-button/LED demonstration, once a DK became
   available in this environment (serial `1051885995`, same board as the
   AWP1-3 evidence): `west flash` succeeded, but the DK's shell UART
   (`/dev/ttyACM1`) produced **no output at all** — not even the boot
   banner — on the first several attempts. Debugging this (via
   `nrfutil device cpu-register-read`/`read --direct` over the SEGGER debug
   probe, no serial needed) uncovered and fixed a real on-target bug that
   predates this evidence being written, not merely a test-environment
   quirk:
   - The DK was permanently halted in `arch_system_halt()`
     (`zephyr/kernel/fatal.c:30`) with `k_fatal_error_reason` `2`
     (`K_ERR_STACK_CHK_FAIL`) and `IPSR`=6 (`UsageFault`, Armv8-M's
     hardware `PSPLIM` stack-limit check). `PSP` equaled `PSPLIM_S` exactly.
     Unwinding the stacked return addresses (`arm-zephyr-eabi-addr2line`)
     showed the main thread — which runs every `SYS_INIT` hook and
     `main()`'s own boot sequencing before the scheduler starts other
     threads — faulted inside `CONFIG_LOG_MODE_IMMEDIATE`'s synchronous
     log-formatting/GRTC-timestamp call chain (`msg_process` ->
     `z_log_msg_runtime_vcreate` -> ... -> `sys_clock_cycle_get_32`).
     `CONFIG_MAIN_STACK_SIZE=4096` (unchanged since AWP1) evidently left
     too little headroom once AWP4 added another `SYS_INIT` hook
     (`authorization_button.cpp`'s `InitAuthorizationButton`); raising it
     to 8192 in `prj.conf` fixed the hang, confirmed by the boot banner and
     shell prompt (`uart:~$`) appearing over `/dev/ttyACM1` immediately
     after reflashing.
   - Independently, `authorization_button.cpp`'s GPIO ISR callback
     (`OnButtonPressed`) called `Window::Open()`, `Indicator::SetActive()`
     (whose first call also does `gpio_pin_configure_dt()`), and `LOG_INF()`
     directly from GPIO-interrupt context — unsafe under
     `CONFIG_LOG_MODE_IMMEDIATE=y`'s blocking UART writes, and a second,
     independent stack-margin risk on the (unrelated) ISR stack even though
     it did not present as the specific fault captured above. Fixed by
     deferring all of it to a `k_work` item processed on the system
     workqueue thread; the ISR now only calls `k_work_submit()`.
   - With both fixes applied and reflashed, the physical demonstration
     was run end to end over the shell (`/dev/ttyACM1`, 115200-8N1,
     hardware flow control required — the debugger auto-detects HWFC from
     the terminal's DTR/RTS and gates TX on CTS if RTS is not asserted):
     - `aliro-ud auth status` → `OK state=required remaining_ms=0`
       (baseline, matching `AliroUd::Authorization::GlobalWindow()`'s
       initial closed state confirmed by a debug-probe RAM read at
       `_ZZN7AliroUd13Authorization12GlobalWindowEvE6window` before any
       button press).
     - A real press of the DK's `Button 0` (`sw0`) was requested from and
       performed by the user; polling `aliro-ud auth status` afterward
       showed `state=authorized remaining_ms=25983`, counting down.
     - A second real press mid-countdown re-extended the window
       (`remaining_ms` jumped from `19680` back up to `28747`), then it
       counted down again to `state=required remaining_ms=0` at expiry —
       exactly the retry/extension and expiry behavior
       `test_authorization_window.cpp` covers on host.
     - `aliro-ud auth notify-required` (new CLI test trigger, added here
       per the "test trigger if the current stack cannot request
       authorization end to end" allowance — no NFC reader was available
       to drive a real AUTH0 exchange) was then run, and LED0
       (`led0`/GPIO1 pin 22) was confirmed lit via a direct GPIO `OUT`
       register read over the debug probe (`0x500D8200`: bit 22 set,
       `0x00450000`) and visually confirmed by the user on the DK.
     - A third real button press was requested and performed; the debug
       probe read confirmed LED0 turned back off (`0x00050000`, bit 22
       clear) and `aliro-ud auth status` reported
       `state=authorized remaining_ms=13348`, matching
       `authorization_button.cpp`'s `Indicator::SetActive(false)` on
       press.

   This satisfies `APP_PLAN.md` AWP4 Verify: "Demonstrate button and LED
   behavior on the DK."

## Verification-method mapping (SyRS codes: T/D/I/A)

- `ALIRO-UD-SYRS-P1-011` — **T**: `authorization_window`'s boundary/retry/
  preauthorization cases plus `authorization_e2e`'s real-AUTH0-through-
  real-window integration. **D**: physical DK button demonstration
  performed (see above) — real presses opened, re-extended, and (by
  waiting) expired the window, cross-checked against `aliro-ud auth
  status`. Updated to `verified-end-to-end`.
- `ALIRO-UD-SYRS-P1-012` — **T**: `authorization_contract`'s
  `NotifyAuthenticationRequired()`/indicator-activation cases and
  `authorization_e2e`'s real-stack indicator-activation case. **D**:
  physical DK LED demonstration performed (see above) via the new
  `aliro-ud auth notify-required` test trigger — LED0 confirmed lit then
  cleared, both by a debug-probe GPIO register read and by the user's
  visual confirmation. Updated to `verified-end-to-end`.
- `ALIRO-UD-SYRS-P1-020` — **T** (application portion only; policy
  *combination* is stack-owned and already covered by
  `stack/tests/user_device/credential/src/test_policy.cpp`): the
  application backend's uniform, policy-value-independent window gate is
  exercised end to end by `authorization_e2e`. Updated to
  `app-implemented`.
- `ALIRO-UD-SYRS-P1-021` — **T**: `test_auth0_policy_0x03_with_no_window_indicates_required`/
  `test_auth0_policy_0x03_with_open_window_does_not_indicate` exercise
  policy 0x03 continuing only with a valid window, through the real stack.
  Updated to `app-implemented`.
- `docs/traceability.md` rows `-004` through `-010` (AWP3, previously left
  `not-yet-verifiable` because `AWP3.md`'s entry was not updated after the
  WP5.5 unblock) are corrected to `app-implemented` in this AWP4 pass; see
  `AWP3.md`'s "Resolution" section for why.

## External stack observations (not blocking AWP4)

- None: the `Authorization` contract used by this AWP (`GetState()`,
  `NotifyAuthenticationRequired()`) is unchanged from AWP2's stub and fully
  sufficient for the required window-based behavior; no adapter gap was
  found.

## Security check

- `grep`-reviewed every new/changed file: no private key, `Kpersistent`, or
  session-key material is read, stored, or logged. `authorization_window.cpp`
  stores only millisecond timestamps; the CLI's `auth status`/`press`/
  `clear` commands never echo anything secret; log lines are
  session-lifecycle-state tokens only (credential handle numbers, window
  open/expiry), matching the existing AWP1-3 log-hygiene bound.

## Outstanding items

- None for AWP4's own scope: both Verify items (unit tests, DK button/LED
  demonstration) are complete, and the on-target crash found while getting
  the DK demonstration running (main-thread stack overflow under
  `CONFIG_LOG_MODE_IMMEDIATE`, `authorization_button.cpp`'s unsafe
  ISR-context work) is fixed and reflashed-and-reverified.
- The `CONFIG_MAIN_STACK_SIZE` fault was only ever observed to manifest
  through this AWP's added `SYS_INIT` hook; it is plausible the margin was
  already thin before AWP4 (immediate-mode logging plus GRTC timestamps is
  not new), so a similar hang is possible on other boot paths that log
  early and heavily. Not re-audited here beyond this AWP's own addition.
- `west-aliro.yml` and `docs/Requirements.pdf` remain untracked/unstaged
  per explicit user instruction; not committed by this pass.
