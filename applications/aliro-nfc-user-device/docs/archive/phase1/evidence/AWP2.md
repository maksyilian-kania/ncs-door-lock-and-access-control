# AWP2 — Development CLI over virtual UART, plus WP5 stub implementations

Part of the per-AWP evidence log (`docs/evidence/`); see `docs/STATE.md` for
the current checkpoint and `../traceability.md` for requirement status.
Previous: `AWP1.md`. Next: `AWP3.md`.

`ncs-aliro` had moved from WP4 (checked out for AWP1) to WP5
(`a7e99c21`, "user_device: WP5: add credential, trust, and
authentication-policy semantics"; confirmed a descendant of the required
baseline `b8bed857`). WP5 makes `Aliro::UserDevice::CredentialManager`
(`stack/src/user_device/credential_manager.cpp`) and
`Aliro::UserDevice::EventHandler::ProcessEvent()`
(`stack/src/user_device/event_handler.cpp`) call
`Aliro::Interface::UserDevice::Credential::{ResolveByReaderGroupIdentifier,
GetMetadata}` and `Authorization::{GetState, NotifyAuthenticationRequired}`
unconditionally, which this application had never implemented (only the
AWP0 `src/storage/credential` and `src/platform/authorization` skeleton
`CMakeLists.txt` placeholders existed) — the application and every test
that links the real stack failed to link.

## Stub implementations added (not AWP2 scope; needed only to compile/link)

1. **`src/storage/credential/credential.cpp`** — placeholder for the full
   `Aliro::Interface::UserDevice::Credential` and `::Trust` contracts.
   `ResolveByReaderGroupIdentifier()` returns `ALIRO_NO_ERROR` with
   `inOutCount = 0` (an empty credential set is a well-defined, successful
   "zero matches" outcome per the contract's own documentation, letting
   `CredentialManager::EvaluateAuth0()` resolve to `Auth0Outcome::kNoMatch`
   cleanly). Every other function (`Validate`, `Create`, `Update`, `Delete`,
   `Reset`, `GetGroupBinding`, `GetMetadata`,
   `Trust::GetReaderPublicKey`/`GetReaderIssuerPublicKey`) returns a real
   error (`ALIRO_ERROR_NOT_IMPLEMENTED` or `ALIRO_PUBLIC_KEY_NOT_FOUND`)
   rather than being omitted, so any future caller fails loudly instead of
   a missing-symbol link error. AWP3 replaces this file's contents with
   real Zephyr settings/NVS-backed persistence.
2. **`src/platform/authorization/authorization.cpp`** — placeholder for
   `Aliro::Interface::UserDevice::Authorization`. `GetState()` always
   returns `AuthorizationState::Required` (fails closed: with no DK
   button/LED backend yet, a credential that requires authorization can
   never be falsely granted it). `NotifyAuthenticationRequired()` only
   logs. AWP4 replaces this file's contents with the real DK button/LED
   implementation.
3. Both files were also added to
   `tests/functional/subsys/aliro_nfc_user_device/worker_lifecycle/CMakeLists.txt`,
   since that test links the same real `Aliro::UserDeviceStack`.

## AWP2 deliverables

1. **Shell enabled on the DK virtual UART.** `prj.conf`: added
   `CONFIG_SHELL=y`. No board overlay/conf changes were needed:
   `boards/nordic/nrf54lm20dk/nrf54lm20dk_common.dtsi` already sets
   `current-speed = <115200>` on the console UART, and Zephyr's UART
   default framing is already 8 data bits / no parity / 1 stop bit
   (8N1), matching APP_PLAN.md AWP2's "115200-8N1" requirement.
2. **`aliro-ud` root command and `info` command.** New
   `src/cli/cli.cpp`. Registered by hand (mirroring
   `SHELL_CMD_ARG_REGISTER()`'s expansion) rather than through that macro
   directly, because the macro token-pastes the `syntax` argument into
   generated variable names, which is impossible for a hyphenated syntax
   like `aliro-ud`; `SHELL_CMD_ARG()`'s own use of `STRINGIFY()` has no
   such restriction, so the actual command text is unaffected. `info`
   prints one deterministic line:
   `OK version=<APP_VERSION_EXTENDED_STRING> init=<state>
   session_active=<0|1> activation_attempts=<n> rejected_apdus=<n>`,
   sourced from a new tiny `src/platform/os/app_status.{h,cpp}` module
   (`main()`-only writer, any-thread reader) for build/initialization
   state and the existing `platform/nfc/nfc_worker.h` diagnostic
   accessors (`IsSessionActive()`, `GetActivationAttemptCount()`,
   `GetRejectedApduCount()`) for session state. A new
   `applications/aliro-nfc-user-device/VERSION` file was added so Zephyr
   generates `APP_VERSION_EXTENDED_STRING` (there was none before; without
   it `<app_version.h>` is not generated).
3. **Credential staging command shells.** `aliro-ud credential
   begin-create|begin-update|set-key|set-binding|set-policy|set-mailbox|
   set-credential-timestamp|set-revocation-timestamp|commit|abort`,
   matching the field categories APP_PLAN.md AWP3 names ("field commands
   set/clear the raw private-key input, binding entries, signed
   timestamps, mailbox configuration"). Every one of these is syntax/
   argument-shape only: each unconditionally prints
   `ERR NOT_IMPLEMENTED command=<name>` and touches no storage or stack
   state; AWP3 supplies the one in-memory staging transaction and its
   real behavior. Inspection, deletion, selection, trust-binding,
   document, and mailbox subcommands are intentionally not added
   (reserved for AWP3/AWP6 per APP_PLAN.md AWP2 scope).
4. Every command above prints exactly one line, prefixed `OK`/`ERR`,
   and never a secret value (the staging shells cannot leak anything —
   they do not read back staged input at all yet).

## Host test added

`tests/functional/subsys/aliro_nfc_user_device/cli_info/` (3 test cases,
native shell/dummy backend, `shell_execute_cmd()` against the real
`cli.cpp` in front of the real `platform/nfc`/`platform/os` code and real
`Aliro::UserDeviceStack`, substituting the worker_lifecycle test's
`fake_nfc_interface.cpp` for the hardware-dependent transport, same as
that test):

- `test_info_reports_ok_line` — `aliro-ud info` returns one `OK` line with
  the expected `version=`/`init=`/`session_active=`/
  `activation_attempts=`/`rejected_apdus=` fields, including
  `init=not_started` (this test never calls `AppStatus::SetInitState()`,
  matching that main() alone owns it).
- `test_info_reflects_live_session_state` — after `PostFieldOn()`, `info`
  reports `session_active=1` and `activation_attempts=1`, proving the
  fields are live, not canned.
- `test_credential_shells_are_not_yet_implemented` — every credential
  staging subcommand returns `ERR NOT_IMPLEMENTED`.

## Commands run and results

Toolchain invoked via `ncs4`, from `/home/mak5-local/gesture-access` (west
topdir).

1. `west build -b nrf54lm20dk/nrf54lm20b/cpuapp applications/aliro-nfc-user-device --pristine`
   — **Result: pass**, both before the CLI was added (to confirm the WP5
   link failure and then the stub fix) and after. Final: FLASH 91856 B
   (4.41%), RAM 42424 B (8.11%) (up from the post-AWP1-hardening-pass
   60544 B/37616 B; the increase is `CONFIG_SHELL`, its dependencies, and
   `src/cli/cli.cpp`).
2. `west twister -T tests/functional/subsys/aliro_nfc_user_device -p native_sim/native/64`
   — **Result: pass.** 4 of 4 test configurations, 23/23 test cases
   (`host_smoke`, `apdu_fragment_assembler`, `worker_lifecycle` [12],
   `cli_info` [3] — new).
3. `west twister -T applications/aliro-nfc-user-device -p nrf54lm20dk/nrf54lm20b/cpuapp`
   — **Result: pass** (`build.aliro_nfc_user_device`, built-only, no
   hardware runner configured in Twister).
4. Physical DK demonstration (once a DK became available in this
   environment, serial 1051885995, same board as the AWP1 pass):
   `west flash` succeeded (`nrfutil` runner, "Board(s) with serial
   number(s) 1051885995 flashed successfully"). Connected to the DK's
   second virtual COM port (`/dev/ttyACM1`, the shell UART; `/dev/ttyACM0`
   carries no shell traffic) at 115200-8N1 and confirmed the Zephyr shell
   prompt (`uart:~$`) and, over that same session:
   - `aliro-ud info` → `OK version=0.2.0-awp2+0 init=running
     session_active=0 activation_attempts=0 rejected_apdus=0`
   - `aliro-ud credential begin-create` → `ERR NOT_IMPLEMENTED
     command=credential begin-create`
   - `aliro-ud credential set-key 00` → `ERR NOT_IMPLEMENTED
     command=credential set-key`
   - `aliro-ud credential commit` → `ERR NOT_IMPLEMENTED
     command=credential commit`
   - `aliro-ud credential abort` → `ERR NOT_IMPLEMENTED
     command=credential abort`
   - `aliro-ud` (no subcommand) → prints the `info`/`credential`
     subcommand help, confirming the command tree
   - `help` → confirms `aliro-ud` is listed as a top-level shell command
     alongside the Zephyr-builtin ones (`app`, `date`, `device`, ...)

   This directly satisfies APP_PLAN.md AWP2's "On the DK, demonstrate
   command input and deterministic output at 115200-8N1" verification
   step.

## Security check

- `grep`-reviewed every new/changed file: no private key, `Kpersistent`,
  or session-key material is read, stored, or logged. The credential
  staging shells accept but never echo back their arguments (they do
  nothing with them yet), and `info` only reports non-secret counters/
  state tokens.

## Outstanding items

- `west-aliro.yml` and `docs/Requirements.pdf` remain untracked/unstaged
  per explicit user instruction; not committed by this pass.
