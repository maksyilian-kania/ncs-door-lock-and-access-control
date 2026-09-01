# Aliro NFC User Device — Evidence Log

Created in AWP0 per `APP_PLAN.md` §1; updated in every subsequent AWP.

## AWP0 — Replace POC logic with the application skeleton

### Baseline

- West topdir: `/home/mak5-local/gesture-access` (manifest file `west-aliro.yml`,
  `.west/config` → `manifest.file = west-aliro.yml`).
- `ncs-aliro` checked-out revision:
  `git -C <west-topdir>/ncs-aliro rev-parse HEAD` →
  `b8bed857b482d288168185e76d5452469739fbdd`.
- This is exactly the minimum baseline required by `APP_PLAN.md` §"Stack-version
  independence" (`b8bed857b482d288168185e76d5452469739fbdd`); no reset or
  `west update` was performed.
- `git -C <west-topdir>/ncs-aliro log --oneline -5`:
  `b8bed857 user_device: WP3: add APDU responder, SELECT codec, and minimal HSM`,
  `a71d4d90 WP2`, `85fba1da`, `c1fa421d WP0`, `ff5094c3 WP1`.

### Public contract inspected

- `include/aliro/user_device/user_device.h` — `Aliro::UserDeviceStack` facade
  (`Instance()`, `Init()`, `ActivateSession()`, `DeactivateSession()`,
  `HandleCommandApdu()`, `ProcessEvent()`).
- `include/aliro/user_device/interface.h` — `Aliro::Interface::UserDevice::*`
  application/platform contract (Nfc, Credential, Trust, CredentialSigning,
  Crypto, Authorization, Mailbox, Os).
- `Kconfig.aliro_user_device` — `NCS_ALIRO_USER_DEVICE` menuconfig, selects
  `NFC_T4T_APDU`, `SMF`, `SMF_ANCESTOR_SUPPORT`, `SMF_INITIAL_TRANSITION`;
  `NCS_ALIRO_USER_DEVICE_SRC` builds the stack from source (default `y`).
- `Kconfig` / `CMakeLists.txt` (repository root) and `zephyr/module.yaml` —
  confirm the module supplies its own Kconfig/CMake integration
  (`zephyr_library_named(aliro)`, auto-linked into the final image); no manual
  `target_link_libraries()` wiring was added to the application.
- `stack/CMakeLists.txt` — confirms `src/user_device` is only added under
  `CONFIG_NCS_ALIRO_USER_DEVICE_SRC`, and that `src/errors` (implementing
  `AliroError::ToString()`) is only added under `CONFIG_NCS_ALIRO` (Reader
  role); see "External stack observations" below.

### Deliverables completed

- Removed AID matching, APDU inspection, and the placeholder `6A82` response
  from `src/main.c` (deleted; replaced by `src/main.cpp`, boot sequencing
  only).
- Removed System OFF scheduling (`k_work_delayable`, `sys_poweroff()`),
  wake-reason handling (`print_reset_reason()`/RESETREAS), and the board
  overlay/`.conf` that only existed to suspend the UART for System OFF
  (`boards/nrf54lm20dk_nrf54lm20b_cpuapp.{conf,overlay}`, deleted). The
  application now remains powered while idle; no `CONFIG_POWEROFF`,
  `CONFIG_PM_DEVICE`, or `CONFIG_PM_DEVICE_RUNTIME`.
- `prj.conf` now enables `CONFIG_NCS_ALIRO_USER_DEVICE=y` only (plus C++17
  toolchain configuration needed to call the C++ stack facade); no
  `NFC_T4T_NRFXLIB`/manual link wiring, since no NFC transport code exists yet
  (AWP1) and the Kconfig/CMake integration is supplied by the module itself.
- Created `src/platform/{nfc,os,crypto,authorization}`,
  `src/storage/{credential,mailbox}`, and `src/cli`, each with a `CMakeLists.txt`
  documenting the AWP that will populate it; wired into the top-level
  `CMakeLists.txt` via `add_subdirectory()`. No Reader-only sources were
  reused.
- Created this file and `docs/traceability.md`.
- Copied `docs/Requirements.pdf` (Aliro NFC User Device SyRS,
  `ALIRO-UD-SYRS-P1-001`..`-040`, `ALIRO-UD-SYRS-P2-001`..`-032`) into the
  application docs directory; it is `APP_PLAN.md`'s authoritative input #2
  and was not previously present in this checkout.
- Fixed a pre-existing Twister schema error in `sample.yaml` (`build_only`
  was nested under `sample:`, which is not a valid key there per the current
  Twister schema); moved it under `common:`, matching the convention used by
  other applications in this repository (e.g.
  `applications/aliro-access-control-app`).

### Commands run and results

Toolchain invoked via `ncs4` (`nrfutil sdk-manager toolchain launch --ncs-version v3.4.0 -- ...`),
from `/home/mak5-local/gesture-access` (west topdir).

1. `west list` — confirmed `ncs-aliro` resolves from the manifest
   (`user-device-dev` revision ref, pinned at the commit above) alongside
   this repository as the manifest project. **Result: pass.**
2. `west build -b nrf54lm20dk/nrf54lm20b/cpuapp -d build/aliro_nfc_user_device_awp0 applications/aliro-nfc-user-device`
   — DK target build. **Result: pass.** FLASH 36832 B (1.77%), RAM 9184 B
   (1.76%).
3. `west build -b native_sim/native/64 -d build/aliro_nfc_user_device_host_smoke tests/functional/subsys/aliro_nfc_user_device/host_smoke`
   then running the produced `zephyr.exe` — host smoke build links
   `Aliro::UserDeviceStack` and calls `Init()` through the current public API.
   **Result: pass** (`ZTEST` suite `aliro_nfc_user_device_host_smoke`,
   1/1 passed).
   - `native_sim/native` (32-bit) failed to build on this host with
     `fatal error: bits/libc-header-start.h: No such file or directory`
     (missing 32-bit glibc multilib headers on the host toolchain, not an
     application or stack defect). `native_sim/native/64` is the
     Zephyr-supported 64-bit variant for hosts without 32-bit multilib and
     was used instead; `testcase.yaml` pins `platform_allow` accordingly.
4. `west twister -T applications/aliro-nfc-user-device -T tests/functional/subsys/aliro_nfc_user_device --outdir twister-out-awp0`
   — **Result:** `build.aliro_nfc_user_device` (nrf54lm20dk/nrf54lm20b/cpuapp,
   build-only) NOT RUN/built successfully;
   `aliro_nfc_user_device.functional.host_smoke` (native_sim/native/64)
   PASSED. 1 of 1 executed test cases passed (100%).
5. Clean-checkout confirmation: the `ncs-aliro` project resolved by `west
   list` above, at the pinned baseline revision, is exactly what the DK and
   host builds in steps 2–4 linked against — i.e. a manifest checkout at this
   commit resolves a User Device stack at the minimum baseline. No `west
   update` was run.

### Verification-method mapping (SyRS codes: T/D/I/A)

- `ALIRO-UD-SYRS-P1-001` — **I** (inspection of build artifacts/config) and
  partial **D**: build evidence only (steps 2 and 4 above). Flashing to a
  physical DK with the PCA64110 antenna and a hardware demonstration were not
  performed in this invocation (no DK hardware access in this environment).
  Recorded as `not-yet-verifiable` in `docs/traceability.md` pending that
  hardware step; re-run per `APP_PLAN.md` §4 once available.
- No other Phase 1 requirement has any implementation in AWP0, per
  `APP_PLAN.md` ("No protocol requirement is complete in this package.").

### External stack observations (not blocking AWP0)

- `AliroError::ToString()` is declared in the shared `aliro/errors.h` header
  used by both roles, but its only definition
  (`stack/src/errors/errors.cpp`) is added to the `aliro` library exclusively
  under `CONFIG_NCS_ALIRO` (Reader) in `stack/CMakeLists.txt`
  (`add_subdirectory(src/errors)` is inside the `if(CONFIG_NCS_ALIRO)` block,
  not the unconditional/User-Device block). A User-Device-only build that
  calls `AliroError::ToString()` fails to link. This did not block AWP0:
  `src/main.cpp` logs `AliroError::ToInt()` instead, which is header-only.
  Flagged here for later AWPs (e.g. AWP2 CLI diagnostics) that may want
  human-readable error strings; no `ncs-aliro` changes were made and no
  application-side duplication of the error-string table was added to work
  around it.

### Security check

- `grep`-reviewed all files touched in this AWP: no private key,
  `Kpersistent`, or session-key material appears in source, CLI output, or
  logs (none of that state exists yet in this skeleton).

### Outstanding items for future AWPs

- Flash-and-boot demonstration on physical nRF54LM20B DK hardware for
  `ALIRO-UD-SYRS-P1-001` (blocked on hardware access in this environment;
  ask the user before marking AWP0 complete without it, per `APP_PLAN.md`
  §"Invocation, repository, and commit contract").

### Manifest note (not committed by this AWP)

The working tree's `west-aliro.yml` was already locally modified, before this
invocation started, to point `ncs-aliro` at `https://github.com/frun36/ncs-aliro`
revision `user-device-dev` (an unpinned branch, currently at the baseline
commit above) instead of the committed
`https://github.com/nrfconnect/ncs-aliro` revision
`b8cd7c5fe7c6f50df4a6bb89a750c1a985e26e0d`. This local edit is what let this
AWP's builds resolve the required baseline; without it, a checkout of the
currently *committed* manifest would fetch a different repository/revision
and would not satisfy "a clean manifest checkout resolves a User Device stack
at or above the minimum baseline." This edit was not made by this AWP and
was left untouched/unstaged (`APP_PLAN.md` §"Invocation, repository, and
commit contract": preserve unrelated unstaged work; modify only files
produced for `TARGET_AWP`). Pointing the committed manifest at a personal
fork and an unpinned branch is a repository/process decision outside this
AWP's scope — ask the user whether/how `west-aliro.yml` should be updated
(e.g. pin `ncs-aliro`'s canonical location and an immutable revision) before
relying on a from-scratch checkout.

Per explicit user instruction, `docs/Requirements.pdf` and any modified
version of `west-aliro.yml` must never be committed by any AWP; both remain
untracked/unstaged in every commit made so far.

## AWP1 — NFC transport, OS bridge, and stack lifecycle

### Baseline

- `ncs-aliro` checked-out revision unchanged from AWP0:
  `b8bed857b482d288168185e76d5452469739fbdd` (`user_device: WP3: add APDU
  responder, SELECT codec, and minimal HSM`). No `west update` was run.

### Public contract inspected

- `include/aliro/user_device/interface.h` — `Aliro::Interface::UserDevice::Nfc`
  (`SendResponseApdu()`, `HandleTermination()`, `GetTimingConstraints()`) and
  `Aliro::Interface::UserDevice::Os` (`QueueEvent()`, `Mutex::Lock/Unlock()`,
  `Timer::Acquire/Release/Start/Stop/IsRunning()`, `GetTrustedTimestamp()`).
- `stack/src/user_device/session_manager.{h,cpp}` — `UserDeviceSessionManager`
  (`kMaxSessions = 1` for P1 NFC-only PICS); `Destroy()` unconditionally calls
  `Interface::UserDevice::Nfc::HandleTermination()`, including for stale/
  duplicate deactivation (documented as a safe no-op when no session exists).
- `stack/src/user_device/session.{h,cpp}` — `UserDeviceSession::HandleCommandApdu()`
  calls `Interface::UserDevice::Nfc::SendResponseApdu()` synchronously, on
  whatever thread called it in; the session watchdog uses
  `Interface::UserDevice::Os::Timer` and defers its expiry through
  `EventHandler::PostEvent()` → `Interface::UserDevice::Os::QueueEvent()`
  (safe to call from ISR/timer context — never touches the stack directly).
- `stack/src/user_device/event_handler.{h,cpp}` — confirms every deferred
  User Device stack event is a `QueueEvent()`-posted opaque pointer, later
  passed unchanged to `UserDeviceStack::ProcessEvent()`.
- `stack/src/user_device/nfc/{command_apdu,apdu_responder}.{h,cpp}`,
  `stack/src/user_device/hsm/state_machine.{h,cpp}`,
  `stack/src/user_device/access_protocol/select_response_encoder.{h,cpp}` —
  confirms the checked-out stack (WP3) already implements Aliro SELECT for
  the Expedited Phase AID end to end, which is why this AWP's tests verify a
  real SELECT response against the spec (see "Deliverables completed" and
  "Commands run" below) instead of only build/link evidence.
- `tests/user_device/dut_adapter/{fake_nfc,fake_timer,fake_queue}.cpp` and
  `tests/user_device/lifecycle/src/test_select.cpp` — the stack's own host
  DUT-adapter fakes and SELECT test; used only as a *reference* for the
  `Interface::UserDevice::Nfc`/`Os` call contract and for the exact
  known-good SELECT command/FCI-response bytes (Aliro 1.0 Specification and
  Test Plan, 26-42802-001, Table 10-3 and §10.2.1.2/Appendix 14.4, page
  96/177), reused verbatim in this AWP's `worker_lifecycle` test. No
  `ncs-aliro` test file was copied or modified.
- `stack/src/aliro/log.h` / `stack/src/user_device/log.h` — both roles'
  `ALIRO_LOG_*`/`ALIRO_UD_LOG_*` macros call
  `Aliro::Interface::Logging::Log()`/`LogHexdump()` unconditionally (declared
  in a private stack header, not gated on any Kconfig). See "External stack
  observations" below: this is a role-neutral platform contract the
  application must implement, not previously needed because AWP0 exercised
  no code path in `stack/src/user_device/*` that logs.

### Deliverables completed

- `src/platform/nfc/nfc_worker.{h,cpp}` — the bounded queue
  (`CONFIG_ALIRO_UD_NFC_EVENT_QUEUE_DEPTH`, default 4) and dedicated
  high-priority thread (`CONFIG_ALIRO_UD_NFC_THREAD_PRIORITY`, default
  `K_PRIO_PREEMPT(5)`) required by `APP_PLAN.md` AWP1. Each queue entry is a
  small, self-contained `WorkerEvent` (kind + optional opaque stack-event
  pointer + an embedded, per-entry command-APDU copy), so a lagging worker
  with several events already queued can never corrupt or race a shared
  buffer. `PostFieldOn()`/`PostFieldOff()`/`PostCommandApdu()`/
  `PostStackEvent()` are the only entry points; all four use `k_msgq_put()`
  with `K_NO_WAIT` (never blocks, safe from ISR/timer context) and return a
  non-zero error when the queue is full, which the caller logs and drops —
  queue overflow, stale events, and duplicate field events are handled by
  dropping/absorbing rather than blocking or corrupting state (see "Commands
  run" below for the test that exercises this).
- `src/platform/nfc/apdu_fragment_assembler.{h,cpp}` — the raw ISO-DEP
  fragment-reassembly state machine (`Incomplete`/`Complete`/`Overflow`),
  factored out of the `nfc_t4t_lib` glue specifically so it can be unit
  tested on `native_sim/native/64` without any NFC hardware dependency
  (`nrfxlib`'s NFC library only ships prebuilt Cortex-M archives, so
  `nfc_transport.cpp` itself cannot build for `native_sim`). Overflow resets
  the assembler and is reported back to the caller (`nfc_transport.cpp`),
  which responds with the transport-layer framing status `6F00` directly —
  not through the stack — keeping `platform/nfc` free of Aliro APDU/TLV
  parsing per `APP_PLAN.md`.
- `src/platform/nfc/nfc_transport.{h,cpp}` — the `nfc_t4t_lib` glue:
  `NfcCallback()` only copies fragments (via `ApduFragmentAssembler`) and
  posts events to the worker (`PostFieldOn/PostFieldOff/PostCommandApdu`);
  it never calls `Aliro::UserDeviceStack` or any stack/storage/crypto
  operation directly. Implements
  `Aliro::Interface::UserDevice::Nfc::SendResponseApdu()` (copies into a
  dedicated transmit buffer retained unchanged until the next
  `nfc_t4t_lib` callback, then calls `nfc_t4t_response_pdu_send()`),
  `HandleTermination()` (resets fragment-assembly state), and
  `GetTimingConstraints()` (returns the default/unconstrained value; the
  exact ALIRO-TP response-time bounds are added in AWP7).
- `src/platform/os/os_mutex.cpp`, `os_timer.cpp`, `os_queue.cpp`,
  `os_trusted_time.cpp` — `Aliro::Interface::UserDevice::Os` implemented
  with Zephyr primitives: `Mutex::Lock/Unlock()` on a `k_mutex` (already
  reentrant for its owning thread); `Timer::*` on a fixed pool of
  `CONFIG_ALIRO_UD_OS_MAX_TIMERS` (default 4) `k_timer`s, whose ISR-context
  expiry handler only forwards to the stack-supplied callback (itself always
  ISR-safe today — it only calls `QueueEvent()`); `QueueEvent()` forwards to
  `AliroUd::Nfc::PostStackEvent()`, so every deferred stack event and every
  NFC transport event is serialized through `UserDeviceStack` calls on the
  *same* worker thread, per `APP_PLAN.md` AWP1 ("Route stack-queued events
  back only to `UserDeviceStack::ProcessEvent()` on the dedicated thread.");
  `GetTrustedTimestamp()` returns `std::nullopt` (no trusted wall clock on
  this platform for P1).
- `src/platform/os/os_logging.cpp` — implements
  `Aliro::Interface::Logging::Log()`/`LogHexdump()`, discovered to be a
  required link-time dependency once any `ALIRO_UD_LOG_*` call site is
  reachable (see "External stack observations" below); gated on
  `CONFIG_NCS_ALIRO_USER_DEVICE_LOG_LEVEL_VALUE`, independently of the
  Reader-scoped equivalent shipped for `aliro-access-control-app`
  (`subsys/aliro/interface_impl/log`, gated on `NCS_ALIRO`/unusable here).
- `src/main.cpp` — after stack `Init()`, calls `AliroUd::Nfc::Start()`
  (starts the worker thread, then `nfc_t4t_setup()` +
  `nfc_t4t_emulation_start()`).
- `prj.conf` — added `CONFIG_NFC_T4T_NRFXLIB=y` (raw ISO-DEP transport, no
  NDEF payload).
- `Kconfig` (application root) and `src/platform/{Kconfig,nfc/Kconfig,os/Kconfig}` —
  added, following the same `rsource` convention as
  `aliro-access-control-app`; expose
  `CONFIG_ALIRO_UD_NFC_THREAD_STACK_SIZE/_PRIORITY/_EVENT_QUEUE_DEPTH` and
  `CONFIG_ALIRO_UD_OS_MAX_TIMERS`, plus per-module `LOG_LEVEL` choices via the
  standard `module =`/`module-str =`/`source "$(ZEPHYR_BASE)/subsys/logging/Kconfig.template.log_config"`
  pattern.
- `tests/functional/subsys/aliro_nfc_user_device/apdu_fragment_assembler/` —
  new host test, no stack dependency: single/chained/zero-length-final
  fragments, single-fragment and cumulative-fragment overflow, recovery
  after overflow, and `Reset()` discarding a stale partial assembly (7 test
  cases).
- `tests/functional/subsys/aliro_nfc_user_device/worker_lifecycle/` — new
  host test: builds the *real* `nfc_worker.cpp` + `platform/os/*.cpp` +
  `Aliro::UserDeviceStack` (not fakes), substituting a small
  `fake_nfc_interface.cpp` test double only for
  `Aliro::Interface::UserDevice::Nfc` (the hardware-dependent
  `nfc_transport.cpp` is excluded, since `nrfxlib`'s NFC library does not
  build for `native_sim`). Covers: field-on → command → exactly one
  response (NFC-to-stack call ordering); the Expedited Phase SELECT command
  returns the exact spec FCI bytes (reused from
  `tests/user_device/lifecycle/src/test_select.cpp`); field-off →
  `HandleTermination()`; duplicate field-on; field-off with no session
  (stale/duplicate deactivation); a field-loss/re-activation race; and
  bounded-queue overflow (posting more events than
  `CONFIG_ALIRO_UD_NFC_EVENT_QUEUE_DEPTH` without letting the worker drain,
  relying on the test thread's higher scheduling priority than the worker
  for determinism) followed by a recovery check (7 test cases).
- `docs/traceability.md` — updated `-001` to `verified-end-to-end` (DK flash,
  see below), `-002` to `app-implemented` (transport portion), `-013` to
  `verified-end-to-end (host)` (SELECT FCI bytes match spec through the real
  worker + real stack), `-015` to `app-implemented (transport-level
  reassembly only)`; every other row unchanged.

### Commands run and results

Toolchain invoked via `ncs4`, from `/home/mak5-local/gesture-access` (west
topdir).

1. `west build -b nrf54lm20dk/nrf54lm20b/cpuapp -d build_awp1_dk applications/aliro-nfc-user-device --pristine`
   — **Result: pass.** FLASH 55968 B (2.68%), RAM 19128 B (3.66%).
2. `west twister -T tests/functional/subsys/aliro_nfc_user_device/apdu_fragment_assembler -p native_sim/native/64`
   — **Result: pass.** 7/7 test cases passed.
3. `west twister -T tests/functional/subsys/aliro_nfc_user_device/worker_lifecycle -p native_sim/native/64`
   — **Result: pass.** 7/7 test cases passed.
4. `west twister -T tests/functional/subsys/aliro_nfc_user_device -T applications/aliro-nfc-user-device -p native_sim/native/64 -p nrf54lm20dk/nrf54lm20b/cpuapp`
   — **Result: pass.** 3 of 4 test configurations executed and passed
   (`host_smoke`, `apdu_fragment_assembler`, `worker_lifecycle`; 15/15 test
   cases), 1 built-only (`build.aliro_nfc_user_device`, DK target, no
   hardware runner configured in Twister).
5. `west flash -d build_awp1_dk` — flashed the AWP1 build to a physical
   nRF54LM20 DK connected to this environment (`nrfutil device list` →
   serial `1051885995`, board `PCA10184`). **Result: pass**
   ("Board(s) with serial number(s) 1051885995 flashed successfully.").
6. Captured the DK's console UART (`/dev/ttyACM1`, 115200 8N1) across a
   device reset:

   ```
   *** Booting nRF Connect SDK v3.4.0-99553055607b ***
   *** Using Zephyr OS v4.4.0-bf801e4e3d19 ***
   [00:03:50.051,170] <inf> aliro_nfc_ud: Aliro NFC User Device
   [00:03:50.057,243] <inf> aliro_nfc_ud: User Device stack initialized
   [00:03:50.064,063] <inf> aliro_ud_nfc: NFC T4T listen mode started (raw ISO-DEP)
   ```

   **Result: pass.** Confirms, on physical hardware: the stack initializes,
   `AliroUd::Nfc::Start()` succeeds, and `nfc_t4t_setup()` +
   `nfc_t4t_emulation_start()` both return `0`. No physical Aliro
   reader/PCA64110 antenna tap was performed in this invocation (no reader
   hardware available); `-002`/`-013` are marked `verified-end-to-end (host)`
   pending that hardware step, not a full field test.

### Verification-method mapping (SyRS codes: T/D/I/A)

- `ALIRO-UD-SYRS-P1-001` — **D**: physical DK flash-and-boot demonstration
  (step 5–6 above) supersedes AWP0's build-only evidence. Updated to
  `verified-end-to-end` in `docs/traceability.md`.
- `ALIRO-UD-SYRS-P1-002` — **T**/partial **D**: the application-owned NFC-A
  Type 4 Tag/ISO-DEP listen-mode transport (fragment assembly, queue,
  worker thread, session lifecycle mapping) is implemented and host-tested
  (`worker_lifecycle`), and confirmed starting on the physical DK (step 6).
  The stack-owned SELECT/ISO-DEP session behavior itself is WP3's, already
  covered by `ncs-aliro`'s own tests; not duplicated here beyond the
  end-to-end SELECT check below. Updated to `app-implemented`.
- `ALIRO-UD-SYRS-P1-013` — **T**: `worker_lifecycle::test_select_expedited_aid_returns_spec_fci`
  sends the Expedited Phase AID SELECT through the real worker thread and
  real stack and asserts the exact FCI bytes from the spec's worked example
  (Aliro 1.0 Specification and Test Plan, 26-42802-001, §10.2.1.2, Appendix
  14.4, page 177). Updated to `verified-end-to-end (host)`; a physical
  reader/antenna tap is still pending.
- `ALIRO-UD-SYRS-P1-015` — **T**: `apdu_fragment_assembler` covers the
  transport-level (raw ISO-DEP) chaining/reassembly this AWP owns.
  Aliro-level command chaining (AUTH0/LOAD CERT/AUTH1/EXCHANGE) is stack-owned
  and out of scope; updated to `app-implemented (transport-level reassembly
  only)`.
- No other Phase 1 requirement has any implementation in AWP1.

### External stack observations (not blocking AWP1)

- `Aliro::Interface::Logging::Log()`/`LogHexdump()` is a role-neutral
  platform contract: `stack/src/aliro/log.h` (included by
  `stack/src/user_device/log.h`) declares and calls it unconditionally for
  every `ALIRO_UD_LOG_*` call site, regardless of role/Kconfig. AWP0 never
  linked this symbol because it never reached any `stack/src/user_device/*`
  code path that logs; this AWP's `UserDeviceSessionManager`/
  `UserDeviceSession` calls do, and the DK build failed at link time with
  `undefined reference to Aliro::Interface::Logging::Log(...)` until
  `src/platform/os/os_logging.cpp` was added. The public
  `include/aliro/interface.h` only declares this namespace under
  `#if CONFIG_NCS_ALIRO_LOG_LEVEL_VALUE > 0` (Reader-only) — not usable for
  a User-Device-only build, where that Kconfig symbol does not exist at
  all — so `os_logging.cpp` declares matching signatures locally instead of
  including that header, mirroring the private
  `stack/src/aliro/log.h` declarations that `ALIRO_UD_LOG_*` actually calls.
  No `ncs-aliro` changes were made; flagged here in case a future `ncs-aliro`
  revision adds a `CONFIG_NCS_ALIRO_USER_DEVICE_LOG_LEVEL_VALUE`-gated
  declaration of this namespace to the public header, which would let this
  app include it directly instead of re-declaring.

### Security check

- `grep`-reviewed all files touched in this AWP: no private key,
  `Kpersistent`, or session-key material is read, stored, or logged (no
  credential/crypto/authorization code exists yet; AWP1 is transport/OS
  bridging only). Command APDU/response bytes are logged only at `LOG_ERR`
  on the transport-layer framing-overflow path, and never at a level that
  would include field/session key material once it exists (revisit this
  bound in AWP5/AWP6 when protected AUTH1/EXCHANGE payloads exist).

### Outstanding items for future AWPs

- Physical Aliro reader/PCA64110 antenna tap for `-002`/`-013` (this
  invocation confirmed NFC T4T listen mode starts on the DK, but no reader
  hardware was available to perform an actual field test).
- `ALIRO-UD-SYRS-P1-040` (NFC/processing-time bounds): `GetTimingConstraints()`
  currently returns the default/unconstrained value; AWP7 adds the exact
  ALIRO-TP bounds and on-target measurement.

## Post-AWP1 — NFC lifecycle-race hardening pass

Not a numbered AWP: a defect-fix pass over `platform/nfc` (AWP1 deliverable)
addressing 8 issues found by analysis of the AWP1 implementation.

### Baseline

- West topdir: `/home/mak5-local/gesture-access` (manifest file `west-aliro.yml`).
- `ncs-aliro` checked-out revision at the time of this pass:
  `80da2641` (`user_device: WP4: add complementary Access Protocol codecs and
  HSM sequencing`), a descendant of the minimum baseline
  `b8bed857b482d288168185e76d5452469739fbdd`
  (`git -C <west-topdir>/ncs-aliro merge-base --is-ancestor b8bed857... HEAD`
  confirmed). The sibling checkout had drifted to an unrelated older commit
  before this pass; it was restored to a valid descendant of the recorded
  baseline (not a `west update`) before building. `session_manager.h`,
  `event_handler.h`, and `include/aliro/user_device/interface.h` were
  diffed against the exact `b8bed857` baseline and are unchanged, so no
  re-audit of the public contract was needed.

### Issues addressed (`src/platform/nfc/nfc_worker.{h,cpp}`, `nfc_transport.cpp`)

1. **Idempotent activation.** The worker now tracks a local
   `sSessionActive` flag. `FIELD_ON` only calls `ActivateSession()` while
   inactive, and only sets the flag after a successful call; a duplicate
   `FIELD_ON` is logged and ignored with zero additional stack calls
   (`GetActivationAttemptCount()` proves this in tests).
2. **Idempotent deactivation.** `FIELD_OFF` only calls
   `DeactivateSession()` while active; a stale/duplicate `FIELD_OFF` is a
   logged no-op, removing the startup "session not found" warning.
3. **Stack-driven termination synchronization.**
   `Aliro::Interface::UserDevice::Nfc::HandleTermination()` now calls the
   new `AliroUd::Nfc::NotifySessionTerminated()`, which clears the local
   flag whenever the stack ends a session independently (watchdog
   timeout), confirmed always to run on the worker thread.
4. **APDUs without an active session.** `HandleEvent()` now checks
   `sSessionActive` before calling `HandleCommandApdu()`; an APDU received
   while inactive is logged, counted (`GetRejectedApduCount()`), and
   dropped without reaching the stack.
5. **No more silently-dropped lifecycle events.** `FIELD_ON`/`FIELD_OFF`
   no longer share the bounded `k_msgq` with command APDUs/stack events;
   they are coalesced onto a dedicated always-succeeding pending-intent
   slot (`sPendingField`) signalled through a semaphore, multiplexed with
   the bounded queue via `k_poll()`. A `CommandApdu`/`StackEvent` that
   cannot be queued (overflow) now sets `sForceRecoveryRequested` and
   forces deterministic teardown of any active session on the worker's
   next wake, instead of silently continuing on skipped data.
6. **`sAssembler` ownership race.** Added `k_spinlock sAssemblerLock` in
   `nfc_transport.cpp`, held across every `sAssembler` access
   (`HandleDataIndication()`'s add/get/reset and `ResetAssembly()`),
   closing the race between the `nfc_t4t_lib` callback thread and the
   worker thread (`HandleTermination()`).
7. **Lifecycle tests.** `tests/functional/subsys/aliro_nfc_user_device/worker_lifecycle`
   now covers, in addition to the AWP1 cases: `ON -> APDUs -> ON -> APDUs ->
   OFF`, `ON -> timeout termination -> ON` (shortened watchdog via
   `CONFIG_NCS_ALIRO_USER_DEVICE_SESSION_TIMEOUT_NFC=500` for this test
   binary only), `ON -> OFF -> ON`, `ON -> queue overflow involving OFF`,
   and APDU rejection both before any `FIELD_ON` and after termination.
   The duplicate-`FIELD_ON` case now asserts
   `GetActivationAttemptCount() == 1`, not just survival.
8. **Diagnostics.** Added symbolic `LOG_INF` lines for all four
   transitions (`FIELD_ON: inactive -> active`, `FIELD_ON: already active,
   ignored`, `FIELD_OFF: active -> inactive`, `STACK_TERMINATION: active ->
   inactive`), plus always-on `IsSessionActive()`,
   `GetActivationAttemptCount()`, `GetRejectedApduCount()` accessors.

### Kconfig/prj.conf changes

- `applications/aliro-nfc-user-device/prj.conf`: added `CONFIG_POLL=y`
  (required by the worker's `k_poll()` multiplexing).
- `tests/functional/subsys/aliro_nfc_user_device/worker_lifecycle/prj.conf`:
  added `CONFIG_POLL=y` and
  `CONFIG_NCS_ALIRO_USER_DEVICE_SESSION_TIMEOUT_NFC=500` (shortened
  watchdog for the timeout-termination test only).

### Commands run and results

Toolchain invoked via `ncs4`, from `/home/mak5-local/gesture-access` (west
topdir).

1. `west twister -T tests/functional/subsys/aliro_nfc_user_device/worker_lifecycle -p native_sim/native/64`
   — **Result: pass.** 12/12 test cases passed (first attempt at a 100 ms
   shortened watchdog timeout spuriously failed 3/12 unrelated cases whose
   idle gaps between events exceeded 100 ms; raised to 500 ms, confirmed
   safely above the largest unrelated idle gap of ~100 ms, and the 3 cases
   passed).
2. `west twister -T tests/functional/subsys/aliro_nfc_user_device -T applications/aliro-nfc-user-device -p native_sim/native/64 -p nrf54lm20dk/nrf54lm20b/cpuapp`
   — **Result: pass.** 3 of 4 test configurations executed and passed
   (`host_smoke`, `apdu_fragment_assembler`, `worker_lifecycle`; 20/20 test
   cases), 1 built-only (`build.aliro_nfc_user_device`, DK target, no
   hardware runner configured in Twister).
3. `west build -b nrf54lm20dk/nrf54lm20b/cpuapp applications/aliro-nfc-user-device --pristine`
   — **Result: pass.** FLASH 60544 B (2.90%), RAM 37616 B (7.19%) (up from
   AWP1's 55968 B/19128 B; the increase is `CONFIG_POLL`/`k_poll()` plus the
   new diagnostics counters and coalesced-field-channel state).
4. No physical DK flash/reader tap was performed for this pass (no new
   hardware-observable behavior; the AWP1 DK boot evidence above still
   applies unchanged).

### Security check

- `grep`-reviewed all files touched in this pass: no private key,
  `Kpersistent`, or session-key material is read, stored, or logged. New
  log lines are session-lifecycle-state only (symbolic event names, no
  APDU/key bytes beyond the pre-existing framing-overflow `LOG_ERR`).

### Outstanding items

- `west-aliro.yml` and `docs/Requirements.pdf` remain untracked/unstaged
  per explicit user instruction; not committed by this pass.
- No commit was made for this pass; changes are left in the working tree
  for the user to review and resume AWP work on top of.

## AWP2 — Development CLI over virtual UART, plus WP5 stub implementations

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

### Stub implementations added (not AWP2 scope; needed only to compile/link)

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

### AWP2 deliverables

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

### Host test added

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

### Commands run and results

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

### Security check

- `grep`-reviewed every new/changed file: no private key, `Kpersistent`,
  or session-key material is read, stored, or logged. The credential
  staging shells accept but never echo back their arguments (they do
  nothing with them yet), and `info` only reports non-secret counters/
  state tokens.

### Outstanding items

- `west-aliro.yml` and `docs/Requirements.pdf` remain untracked/unstaged
  per explicit user instruction; not committed by this pass.

## AWP3 — Credential and trust persistence (stopped, not committed)

### Baseline

- `ncs-aliro` checked-out revision unchanged since AWP2:
  `a7e99c21` ("WP5: add credential, trust, and authentication-policy
  semantics"), confirmed a descendant of the minimum baseline `b8bed857`
  (`git -C <west-topdir>/ncs-aliro merge-base --is-ancestor b8bed857... HEAD`).
  No `west update` was run; only branch present besides the detached HEAD is
  `manifest-rev`.

### Public contract inspected

- `include/aliro/user_device/interface.h` —
  `Aliro::Interface::UserDevice::Trust::GetReaderPublicKey(CredentialHandle,
  CryptoTypes::PublicKey&)` and `::GetReaderIssuerPublicKey(CredentialHandle,
  CryptoTypes::PublicKey&)`: both take only a `CredentialHandle`, with no
  `reader_group_identifier`/binding index parameter.
- `stack/src/user_device/credential_manager.{h,cpp}` —
  `CredentialManager::ResolveReaderTrust(CredentialHandle handle)` is the
  only caller of either function in the checked-out stack, and it forwards
  only the handle it received, at lines 72 and 79 of `credential_manager.cpp`.
  `ResolveReaderTrust()` is not yet called from anywhere else (WP5 declares
  it; WP6 is expected to wire it into AUTH1 orchestration).
- Confirmed no other `ncs-aliro` branch or later commit changes this
  signature: `git branch -a` shows only `manifest-rev` besides the detached
  `a7e99c21` HEAD; `git log --all --oneline | grep -i "trust\|binding"`
  finds only `a7e99c21` itself and `4cf8bc57` ("aliro: added trust
  framework"), an already-ancestored Reader-role commit unrelated to the
  User Device `Trust` contract.

### Blocker: `Trust` contract is not binding-aware

`APP_PLAN.md` AWP3 requires modeling every trust binding as
`reader_group_identifier + trust_type + reader_group_identifier_key`, and
explicitly states "Multiple bindings on one credential may use different
keys." A `CredentialHandle`-only `Trust::GetReaderPublicKey()`/
`GetReaderIssuerPublicKey()` signature gives the application-owned backend
no way to know which of a credential's (up to 16) bindings matched the
`reader_group_identifier` received in AUTH0, so it cannot return the correct
per-binding key once a credential has more than one binding with different
keys. `APP_PLAN.md` explicitly forbids working around this ("Do not cache a
'last resolved identifier' or force bindings to share a key") and instructs:
"Stop AWP3 without committing and report the required binding-aware stack
contract unless the checked-out public API has been corrected." The public
API has not been corrected since this exact gap was first flagged in the
AWP2 evidence entry above (`docs/traceability.md` row `-010`).

**No `src/storage/credential`, `src/storage/mailbox`, or CLI-provisioning
code was implemented for AWP3. No files were changed or committed by this
invocation.** `docs/traceability.md` is unchanged (rows `-004` through
`-010` remain `not-yet-verifiable`).

### Required external contract change

`Aliro::Interface::UserDevice::Trust::GetReaderPublicKey()` and
`::GetReaderIssuerPublicKey()` need a way to identify which binding the
stack is resolving trust for — for example, accepting the
`ReaderGroupIdentifier` (already available to `CredentialManager` from the
AUTH0 command) in addition to the `CredentialHandle`, so the application
backend can look up the specific `reader_group_identifier_key` provisioned
for that `reader_group_identifier + trust_type` pair on that credential,
rather than an ambiguous "the" key for the whole credential.

### Outstanding items

- Blocked on the `ncs-aliro` public `Trust` contract becoming
  binding-aware (see above). Ask the user before continuing AWP3 further:
  whether to wait for an upstream `ncs-aliro` fix, or how they want this
  handled.

### Resolution (out of band, before AWP4)

`ncs-aliro` moved to `37c465e8` ("WP5.5: partial pre-WP6 User Device
remediation (D1-D9, unblock AWP3)") and then `fa606452` ("WP5.5: complete
WP5.5 remediation"), both confirmed descendants of the required baseline
`b8bed857`. WP5.5 changed `Trust::GetReaderPublicKey()`/
`GetReaderIssuerPublicKey()` to accept a `ReaderGroupIdentifier` alongside
the `CredentialHandle` (decision D9), resolving the exact blocker recorded
above. AWP3 was subsequently implemented in full against the corrected
contract: `src/storage/credential/{credential_store,credential_persistence_settings,key_backend_psa,credential_types,provisioning,credential_persistence,key_backend}`
(Zephyr settings/NVS-backed metadata, PSA/CRACEN/KMU-backed private keys, a
four-phase crash-safe journal, per-binding `{reader_group_identifier,
trust_type, reader_group_identifier_key}` trust storage, preferred-credential
tracking), `src/lifecycle/lifecycle.h` (the mutating-operation coordinator),
and the `aliro-ud credential *` CLI staging/management commands, verified
with a DK build and the `worker_lifecycle`/`cli_info` host suites (commit
"Implement AWP3: credential and trust persistence"). This note corrects
`docs/evidence.md`'s AWP3 entry, whose text was not updated to reflect the
resolution at the time; `docs/traceability.md` rows `-004` through `-010`
are corrected to their actual AWP3-implemented status in this AWP4 entry
below.

## AWP4 — Local authorization and visible indication

### Baseline

- West topdir: `/home/mak5-local/gesture-access` (manifest file
  `west-aliro.yml`).
- `ncs-aliro` checked-out revision:
  `fa606452ab587bf7aaae85506962f1e340545471` ("user_device: complete WP5.5
  remediation"), confirmed a descendant of the minimum baseline
  `b8bed857b482d288168185e76d5452469739fbdd`
  (`git -C <west-topdir>/ncs-aliro merge-base --is-ancestor b8bed857... HEAD`).
  No `west update` was run.

### Public contract inspected

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

### Deliverables completed

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

### Commands run and results

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

### Verification-method mapping (SyRS codes: T/D/I/A)

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
  `not-yet-verifiable` because this evidence log's AWP3 entry was not
  updated after the WP5.5 unblock) are corrected to `app-implemented` in
  this AWP4 pass; see "Resolution (out of band, before AWP4)" above for why.

### External stack observations (not blocking AWP4)

- None: the `Authorization` contract used by this AWP (`GetState()`,
  `NotifyAuthenticationRequired()`) is unchanged from AWP2's stub and fully
  sufficient for the required window-based behavior; no adapter gap was
  found.

### Security check

- `grep`-reviewed every new/changed file: no private key, `Kpersistent`, or
  session-key material is read, stored, or logged. `authorization_window.cpp`
  stores only millisecond timestamps; the CLI's `auth status`/`press`/
  `clear` commands never echo anything secret; log lines are
  session-lifecycle-state tokens only (credential handle numbers, window
  open/expiry), matching the existing AWP1-3 log-hygiene bound.

### Outstanding items

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

## AWP5 — PSA cryptography backend

### Baseline

- West topdir: `/home/mak5-local/gesture-access` (manifest file
  `west-aliro.yml`).
- `ncs-aliro` checked-out revision: unchanged from AWP4
  (`fa606452ab587bf7aaae85506962f1e340545471`, "user_device: complete WP5.5
  remediation"). No `west update` was run.

### Public contract inspected

- `include/aliro/user_device/interface.h` —
  `Aliro::Interface::UserDevice::Crypto` (`GenerateRandom`,
  `GenerateEphemeralKeyPair`, `RawKeyAgreement`, `DeriveSymmetricKey`,
  `DeriveRawKey`, `AeadEncrypt`/`AeadDecrypt`, `VerifySignature`, `Sha256`,
  `ValidateCertificate`, `DestroyKey`) and `::CredentialSigning::Sign`.
- `include/aliro/types.h` — `CryptoTypes::{KeyId, PublicKey, PrivateKey,
  Signature, Nonce, AuthenticationTag, Sha256Hash, SharedSecret}` sizes
  (P-256 uncompressed public key = 65 B, signature = 64 B `r||s`, shared
  secret = 32 B, symmetric key = 32 B, nonce = 12 B, tag = 16 B).
- Aliro 1.0 Specification and Test Plan, 26-42802-001, section 13
  ("Reader certificate compression") — the profile0000 DER scheme (fixed
  default `serialNumber`/`issuer`/`notBefore`/`notAfter`/`subject` values,
  `authorityKeyIdentifier` = SHA-1 of the issuer public key per RFC5280
  method 1) and section 14.1's four worked compression examples.

### External stack/pre-existing-gap observation (found during this AWP)

- `key_backend_psa.cpp` (AWP3) and its host-test fake both call
  `psa_import_key()`/`psa_sign_message()` with
  `PSA_KEY_TYPE_ECC_KEY_PAIR(SECP_R1)`/`PSA_ALG_ECDSA(SHA_256)`, but no
  `CONFIG_PSA_WANT_*` symbol for ECC/ECDSA/SHA-256 was ever added to
  `prj.conf` or any `Kconfig` reachable from the application (confirmed by
  inspecting `zephyr/.config` from an existing `build_awp4_dk` build
  directory: `CONFIG_PSA_WANT_ALG_ECDSA`/`_ECC_SECP_R1_256`/`_SHA_256` were
  all unset). This is a latent AWP3 gap, not something introduced here: on
  real hardware `nrf_security`'s `PSA_WANT_*` gating is not optional; only
  `native_sim`'s all-software mbedtls happened to have been exercised via
  test-only `prj.conf` files that already listed these symbols explicitly.
  Fixed in this AWP by adding a `CONFIG_ALIRO_UD_CREDENTIAL_PSA_WANT`
  Kconfig `select`-block to `src/storage/credential/Kconfig` (pattern
  copied from the pre-existing sibling implementation
  `subsys/aliro/crypto_utils/Kconfig`); confirmed corrected by inspecting
  the AWP5 DK build's `zephyr/.config` (see below).

### Deliverables completed

- `src/platform/crypto/crypto.cpp` — `Aliro::Interface::UserDevice::Crypto`:
  thin PSA Crypto bindings for `GenerateRandom` (`psa_generate_random`),
  `GenerateEphemeralKeyPair`/`RawKeyAgreement` (volatile
  `PSA_KEY_TYPE_ECC_KEY_PAIR(SECP_R1)` + `PSA_ALG_ECDH`),
  `DeriveSymmetricKey`/`DeriveRawKey` (`PSA_ALG_HKDF(PSA_ALG_SHA_256)`),
  `AeadEncrypt`/`AeadDecrypt` (`PSA_ALG_GCM`, single-part API),
  `VerifySignature` (import-then-`psa_verify_message`,
  `PSA_ALG_ECDSA(SHA_256)`), `Sha256` (`psa_hash_compute`), `DestroyKey`
  (idempotent `psa_destroy_key`), and `ValidateCertificate` (delegates to
  `certificate.cpp`). Every key here is `PSA_KEY_LIFETIME_VOLATILE`: no
  session/ephemeral key is ever persisted, so this file needs no
  hardware-backed secure-storage driver and runs unchanged on `native_sim`
  and the DK.
- `src/platform/crypto/certificate.{h,cpp}` — Aliro profile0000 Reader
  certificate decompression/validation (`APP_PLAN.md` AWP5, not a thin PSA
  binding): a hand-rolled minimal DER TLV reader/writer parses the
  compressed structure, reconstructs the implicit RFC5280
  `TBSCertificate` bytes from the spec's fixed default field values,
  regenerates the `authorityKeyIdentifier` extension
  (`SHA-1(issuerPublicKey)`), and verifies the reconstructed certificate's
  ECDSA-P256/SHA-256 signature against `issuerPublicKey` before returning
  the subject public key. No wall clock is added; `notBefore`/`notAfter`
  are decoded and shape-validated only, per `APP_PLAN.md`'s explicit
  instruction not to enforce certificate validity dates. Every non-success
  return path yields one of a small fixed set of error codes, independent
  of *why* trust material is absent (`ALIRO-UD-SYRS-P1-024`).
- `src/platform/crypto/credential_signing.cpp` —
  `Aliro::Interface::UserDevice::CredentialSigning::Sign()`: resolves the
  opaque `CredentialHandle` to its `PersistedCredential::mKeyId` via
  `AliroUd::Credential::Store::GetFullRecord()` (AWP3), then signs only
  through `AliroUd::Credential::KeyBackend::Sign()` — the private key
  scalar is never copied into this module or any other.
- `src/storage/credential/key_backend.h` /
  `key_backend_psa.cpp` — added `KeyBackend::Sign()` (real, PSA/CRACEN-KMU-
  or Oberon-backed persistent key, depending on target SoC's `nrf_security`
  driver) so `credential_signing.cpp` never needs to know how an
  application-facing `KeyId` maps to a usable PSA key handle.
- `tests/.../common/fake_key_backend.cpp` — added the matching
  `KeyBackend::Sign()` fake (maps the desired `KeyId` to its internal
  volatile PSA id, then `psa_sign_message()`), for host-test parity.
- `src/platform/crypto/Kconfig` — new `CONFIG_ALIRO_UD_CRYPTO` (`default
  y`) `select`-block declaring every `CONFIG_PSA_WANT_*` symbol this
  module's algorithms/key-types need (random generation; ECDH ephemeral
  key generation; HKDF/HMAC; AES-GCM/AES; ECDSA verify against an imported,
  not generated, public key; SHA-256; SHA-1 for the certificate AKI only).
- `src/storage/credential/Kconfig` — new `CONFIG_ALIRO_UD_CREDENTIAL_PSA_WANT`
  `select`-block for the pre-existing AWP3 gap described above (ECC key
  pair import/export, ECDSA, SHA-256, P-256).
- `applications/aliro-nfc-user-device/src/platform/Kconfig` — `rsource
  "crypto/Kconfig"`; `platform/crypto/CMakeLists.txt` — new module sources.
- `tests/functional/subsys/aliro_nfc_user_device/crypto/` — new host test
  target (17 cases across three suites):
  - `test_crypto.cpp` (9 cases): `GenerateRandom` (fills/varies, rejects
    null-with-nonzero-length, zero-length no-op); ephemeral ECDH agreement
    (both sides derive the identical shared secret — commutativity, not a
    hardcoded vector, since this is a thin binding over already-validated
    mbedtls/PSA primitives); `DeriveSymmetricKey` round-tripped through a
    full `AeadEncrypt`/`AeadDecrypt` cycle plus a tampered-ciphertext
    `ALIRO_INVALID_AUTHENTICATION_TAG` rejection case; `DeriveRawKey`
    determinism for identical `(ikm, salt, info)`; `VerifySignature`
    accepting a locally-generated-and-signed message and rejecting a
    tampered signature/tampered message; `Sha256` against the NIST FIPS
    180-2 known-answer vector `SHA-256("abc")`; `DestroyKey(0)` no-op.
  - `test_certificate.cpp` (5 cases): the Aliro 1.0 Specification's own
    section 14.1 "Demo 1" worked example (all-optional-fields-default
    profile0000 certificate, its issuer CA public key, and its expected
    subject public key) — independently cross-checked before being
    hardcoded into this test: the compressed DER's internal TLV lengths
    were confirmed to sum exactly to the outer `30 81 95` length,
    `SHA-1(issuerPublicKey)` was confirmed to equal the spec's own printed
    `authorityKeyIdentifier` value byte-for-byte, and the spec's
    uncompressed X.509 form of the same certificate was confirmed to have
    a genuinely valid ECDSA signature against that issuer key using
    Python's `cryptography` library, before trusting the vector for this
    test. Also covers: wrong issuer key (subject key cleared on failure),
    tampered signature byte (`ALIRO_INVALID_SIGNATURE`), truncated
    certificate and null certificate (`ALIRO_INVALID_DATA_FORMAT`).
  - `test_credential_signing.cpp` (3 cases): end-to-end against a real
    AWP3 credential provisioned through `credential_store::Create()` (host
    fakes for persistence/key-backend) — `CredentialSigning::Sign()`
    produces a signature independently checked against the credential's
    exported public key via `Crypto::VerifySignature()`; invalid handle and
    empty-data rejection with output cleared on failure.
- `applications/aliro-nfc-user-device/prj.conf` — no direct change needed:
  `CONFIG_PSA_CRYPTO`/`CONFIG_PSA_WANT_*` are now supplied transitively by
  the new `platform/crypto/Kconfig` and `storage/credential/Kconfig`
  `select`-blocks (`default y`, unconditional), consistent with how every
  other per-module `Kconfig` in this application already works.

### Commands run and results

Toolchain invoked via `ncs4`, from `/home/mak5-local/gesture-access` (west
topdir).

1. `west twister -T ncs-door-lock-and-access-control.git/tests/functional/subsys/aliro_nfc_user_device -p native_sim/native/64`
   — **Result: pass.** 6 of 6 test configurations, 59/59 test cases
   (`host_smoke`, `apdu_fragment_assembler`, `worker_lifecycle` [12],
   `authorization` [17], `cli_info` [5], `crypto` [17 — new]).
2. `west build -b nrf54lm20dk/nrf54lm20b/cpuapp -d build_awp5_app_dk ncs-door-lock-and-access-control.git/applications/aliro-nfc-user-device --pristine`
   — **Result: pass.** Final FLASH 156616 B (7.51%), RAM 82120 B (15.69%)
   (unchanged from AWP4's application RAM/FLASH baseline — the crypto
   module's added code is small and every key is volatile/stack-scoped, no
   new persistent RAM state).
3. `west build -b nrf54lm20dk/nrf54lm20b/cpuapp -d build_crypto_dk ncs-door-lock-and-access-control.git/tests/functional/subsys/aliro_nfc_user_device/crypto --pristine`, then `west flash`, then read the shell UART (`/dev/ttyACM1`, 115200-8N1) across a board reset (physical DK, serial `1051885995`, same board as prior AWPs) — **Result: pass, on real hardware.** All 17 host test cases (`aliro_ud_certificate` [5], `aliro_ud_credential_signing` [3], `aliro_ud_crypto` [9]) passed unchanged on-target, running against the DK's real `nrf_security` PSA driver (`libmbedcrypto`/`nrf_oberon` for the nRF54LM20B SoC — confirmed from the build's object list) rather than `native_sim`'s all-software mbedtls. This satisfies `APP_PLAN.md` AWP5 Verify: "Run the vectors on the target DK as well as the host-capable backend and confirm the intended PSA driver path from build/runtime evidence."
   - First attempt faulted on-target (`K_ERR_STACK_CHK_FAIL` in
     `test_sign_produces_signature_verifiable_against_credential_public_key`):
     `PersistedCredential` (two embedded 1024 B `OptionalDocument` buffers
     plus a 16-entry `Binding` array) on the ztest thread's stack overflowed
     `CONFIG_ZTEST_STACK_SIZE=8192`. Fixed by raising it to 16384 in the
     test's `prj.conf`; reconfirmed passing on `native_sim` and the DK
     after the fix.
4. `west build -b nrf54l15dk/nrf54l15/cpuapp -d build_awp5_dk_l15 ncs-door-lock-and-access-control.git/applications/aliro-nfc-user-device --pristine`
   — **Result: pass** (a second SoC family, CRACEN/KMU rather than
   nRF54LM20's Oberon-core driver, exercising the same `CONFIG_PSA_WANT_*`
   selection through a different `nrf_security` backend). Confirmed via
   `zephyr/.config` that `CONFIG_PSA_WANT_ALG_{ECDSA,ECDH,GCM,HKDF,SHA_1,
   SHA_256}`, `CONFIG_PSA_WANT_ECC_SECP_R1_256`, and
   `CONFIG_PSA_WANT_KEY_TYPE_ECC_{KEY_PAIR_IMPORT,KEY_PAIR_GENERATE,PUBLIC_KEY}`
   are all now `y` (previously unset for `ALIRO_UD_CREDENTIAL`'s needs on
   this build, per the pre-existing-gap note above; this build was not
   itself flashed, since the DK physically available in this environment is
   the nRF54LM20 board used in item 3).

### Verification-method mapping (SyRS codes: T/D/I/A)

- `ALIRO-UD-SYRS-P1-008` (session/ephemeral-key portion), `-018`, `-022`
  through `-027`, `-038` — **T**: the new `crypto`/`certificate`/
  `credential_signing` host suites (17 cases) directly exercise every
  primitive these codes require at the application/platform boundary. **D**:
  the same 17 cases were re-run unchanged on a physical nRF54LM20 DK
  against its real PSA driver (item 3 above). Updated to `app-implemented`
  (protocol *orchestration* — which bytes get signed/derived/encrypted and
  in what sequence — remains stack-owned/WP6-pending, per `APP_PLAN.md`'s
  own AWP5 scope note; see `docs/traceability.md`).

### External stack observations (not blocking AWP5)

- None beyond the pre-existing AWP3 `CONFIG_PSA_WANT_*` gap described
  above, which is application-side and has been fixed in this pass.

### Security check

- `grep`-reviewed every new/changed file: no private key scalar,
  `Kpersistent`, or session-key byte is ever logged (log lines carry only
  PSA status codes, key IDs, and lengths); `VerifySignature`/certificate
  validation return one of a small, non-key-dependent set of error codes on
  every failure path, matching `ALIRO-UD-SYRS-P1-024`'s non-disclosure
  requirement. `CredentialSigning::Sign()` never reads `PersistedCredential`
  fields other than `mKeyId`.

### Outstanding items

- None for AWP5's own scope: both Verify items (independently-checked
  known-answer/round-trip vectors, and DK + host execution confirming the
  intended PSA driver path) are complete.
- The DK build in item 4 (nRF54L15, CRACEN/KMU) was not physically flashed
  in this environment (only the nRF54LM20 DK from prior AWPs is attached);
  its `.config` inspection is build/static evidence only, not a runtime
  demonstration on that specific SoC.
- `west-aliro.yml` and `docs/Requirements.pdf` remain untracked/unstaged
  per explicit user instruction; not committed by this pass.

## AWP6 — Mailbox persistence

### Baseline

- West topdir: `/home/mak5-local/gesture-access` (manifest file
  `west-aliro.yml`).
- `ncs-aliro` checked-out revision: unchanged from AWP5. No `west update`
  was run.
- This AWP was split into two passes. The first pass found no physical DK
  attached (`ls /dev/ttyACM*`/`/dev/serial/by-id/` found nothing); the DK
  evidence recorded then was build-only (cross-compile + memory report).
  A second pass, later in the same day, found the DK attached
  (`nrfutil device list` reported serial `1051885995`, the same board as
  every prior AWP) and completed the full interactive DK bring-up
  described below, including finding and fixing a real on-target bug (see
  "On-target bug found and fixed").

### Public contract inspected

- `include/aliro/user_device/interface.h` —
  `Aliro::Interface::UserDevice::Mailbox` (`OpenSnapshot`, `Read`,
  `StageWrite`, `StageSet`, `Commit`, `Rollback`, `Close`), keyed by an
  opaque `SessionHandle`.
- `include/aliro/user_device/types.h` — `MailboxHandle` (opaque
  `uint32_t`, `kInvalidMailboxHandle == 0`). No public contract binds a
  `MailboxHandle` to a `CredentialHandle`; nothing in the checked-out
  stack (`stack/src/user_device/*`) currently constructs or consumes a
  `MailboxHandle` value, confirming this AWP is application-only for
  Phase 1 (one mailbox per Access Credential, `MailboxHandle ==
  CredentialHandle` numerically — an application-level design decision,
  documented in `mailbox_types.h`).
- `src/storage/credential/credential_types.h` (AWP3) —
  `PersistedCredential::mMailbox` (`MailboxConfig{ mConfigured,
  mSizeBytes, mReadable, mWritable, mSettableInAuth1 }`), staged via
  `credential set-mailbox` and committed with the rest of the credential
  transaction. This AWP reads that field but does not change its shape.

### Deliverables completed

- `src/storage/mailbox/mailbox_types.h` — `MailboxRecord` (an
  `mInitialized` flag plus a fixed `kMaxSizeBytes`-byte buffer, one per
  credential slot) and the `MailboxHandle <-> CredentialHandle` mapping
  decision above.
- `src/storage/mailbox/mailbox_persistence.{h,cpp}` — Zephyr
  settings/NVS-backed committed-byte storage, one settings key per
  credential slot (pattern copied from AWP3's
  `credential_persistence_settings.cpp`): `Init`/`LoadSlot`/`SaveSlot`/
  `EraseSlot`.
- `src/storage/mailbox/mailbox_store.{h,cpp}` — the Credential-Issuer-level
  layer (used directly by the CLI, bypassing Reader
  `MailboxPermissions`): `Init` (loads every persisted slot at boot),
  `GetConfig` (live `mSizeBytes`/permissions from `credential_store`, plus
  `mInitialized`), `Initialize` (idempotent lazy zero-fill on first use),
  `Reset` (explicit re-zero), `IsInitialized`/`HasNonZeroData`,
  `RawRead` (bounds-checked against the *current* provisioned size, since
  `set-mailbox`/`credential update` can shrink it), `ApplyDirtyBytes`
  (applies a session's staged buffer+dirty-bitmap to committed storage and
  persists atomically as one `SaveSlot()` call), `EraseForCredential`/
  `EraseAll` (called from the CLI's `credential delete`/`credential
  reset` so mailbox data never survives a `CredentialHandle` being freed
  for reuse — a privacy requirement not explicit in the interface
  contract but implied by "retain ... until explicit deletion").
- `src/storage/mailbox/mailbox_sessions.{h,cpp}` — the Reader-facing,
  permission-enforcing session engine implementing the
  `Aliro::Interface::UserDevice::Mailbox` semantics: `OpenSnapshot`
  (fixed `CONFIG_ALIRO_UD_MAILBOX_MAX_SESSIONS`-entry session table,
  copies the current committed bytes into a session-local shadow buffer
  plus a same-size dirty bitmap, lazily calls `Store::Initialize()` on
  first open), `Read` (served from the *committed* snapshot copy, never
  from staged/dirty bytes — reads are isolated from concurrent staged
  writes in the same or another session), `StageWrite`/`StageSet` (mutate
  only the session's shadow buffer and dirty bitmap; bounds- and
  overflow-checked, `StageWrite` gated on `mWritable`, `StageSet` requires
  the full mailbox length in one call and is also gated on `mWritable`),
  `Commit` (applies the shadow buffer's dirty bytes to committed storage
  via `Store::ApplyDirtyBytes()` in one call — atomic from the caller's
  perspective — then closes the session; on persistence failure, no
  staged changes are applied and the session is left open so the caller
  may retry), `Rollback`/`Close` (discard staged changes, committed bytes
  unchanged), `Read`/`StageWrite`/`StageSet` all reject once the
  mailbox's *current* provisioned size has shrunk below an in-flight
  session's original bounds (re-validated against `Store::GetConfig()` on
  every call, not cached at `OpenSnapshot()` time).
- `src/storage/mailbox/mailbox.cpp` — thin adapter registering
  `Aliro::Interface::UserDevice::Mailbox`'s free functions as direct
  forwards to `AliroUd::Mailbox::Sessions`.
- `src/storage/mailbox/Kconfig` — `CONFIG_ALIRO_UD_MAILBOX_MAX_SESSIONS`
  (default 2, range 1-8): fixed capacity of the
  `OpenSnapshot()`-without-`Close()` session table.
- `src/cli/cli.cpp` — `aliro-ud mailbox inspect|read|init|reset`
  (Credential-Issuer-level commands per `APP_PLAN.md` AWP6, deliberately
  bypassing Reader `MailboxPermissions`, matching Aliro 1.0 Specification
  section 8.3.1.15 p.60: "The mailbox content SHALL be readable and
  writeable by the Credential Issuer"), plus `credential delete`/
  `credential reset` now calling `Mailbox::Store::EraseForCredential()`/
  `EraseAll()` after a successful stack-level delete/reset.
- `src/main.cpp` — `Mailbox::Store::Init()` added to the boot sequence
  after `Credential::Store::Init()`.
- `tests/.../common/fake_mailbox_persistence.{h,cpp}` — in-memory fake for
  `mailbox_persistence.h` with per-slot fault injection (`SetLoadFailure`/
  `SetSaveFailure`/`SetEraseFailure`) and a `SimulateReboot()` that clears
  only non-persisted (RAM-side) state, for crash/reset test scenarios.
- `tests/functional/subsys/aliro_nfc_user_device/mailbox/` — new host test
  target (26 cases across three suites, all against the real
  `mailbox_store.cpp`/`mailbox_sessions.cpp`/`mailbox.cpp` and the real
  `credential_store.cpp` for live mailbox configuration):
  - `test_mailbox_store.cpp` (10 cases): live config reflection from
    credential provisioning, rejecting credentials without a configured
    mailbox or an unknown handle, idempotent `Initialize()`/explicit
    `Reset()` zero-fill semantics, `RawRead()` bounds/overflow rejection,
    `ApplyDirtyBytes()` touching only dirty offsets, `EraseForCredential`/
    `EraseAll`, and persistence across a simulated reboot.
  - `test_mailbox_sessions.cpp` (12 cases): rejecting `OpenSnapshot()` on
    an unconfigured mailbox, lazy committed-storage initialization on
    first open, read isolation from a concurrent session's staged writes
    until commit, rollback/close leaving committed bytes unchanged,
    read-without-`mReadable`/write-without-`mWritable` rejection,
    out-of-bounds/overflowing `StageWrite` ranges, `StageSet` requiring
    the exact full length, independent staging across multiple concurrent
    sessions, safe failure on an unknown/closed session handle, a
    mid-session shrinking-mailbox re-validation case, and one case
    exercising the `Aliro::Interface::UserDevice::Mailbox` adapter
    directly (not just the `Sessions` layer underneath it).
  - `test_mailbox_fault_injection.cpp` (4 cases): a failed `Initialize()`
    leaves the mailbox reporting uninitialized; a failed `Commit()`
    (injected `SaveSlot` failure) leaves previously-committed bytes
    unchanged and the session usable for a retry; a simulated reboot after
    a failed commit recovers the last successfully committed state, not
    the attempted one; a failed `EraseSlot()` leaves the slot's
    in-memory `mInitialized` flag unchanged (erase is not silently
    treated as having succeeded).
- `tests/.../mailbox/Kconfig` and `tests/.../cli_info/Kconfig` — new
  `rsource` lines pulling in `storage/mailbox/Kconfig` (pattern matching
  every other test `Kconfig` in this suite: options normally sourced via
  the application's own `Kconfig` must be re-sourced explicitly for a
  standalone test executable).
- `tests/.../cli_info/CMakeLists.txt` and `src/test_cli_info.cpp` — linked
  the real mailbox sources (`mailbox.cpp`/`mailbox_sessions.cpp`/
  `mailbox_store.cpp`) plus `fake_mailbox_persistence.cpp`, and added
  `test_mailbox_inspect_init_read_and_erase_on_delete`: exercises
  `credential set-mailbox` staging through `mailbox inspect`
  (pre-/post-`init`), `mailbox init`, `mailbox read`, and confirms
  `credential delete` erases the mailbox (`mailbox inspect` on the freed
  handle then fails).
- `applications/aliro-nfc-user-device/prj.conf` —
  `CONFIG_SHELL_STACK_SIZE=16384` (was the Zephyr default of 2048); see
  "On-target bug found and fixed" below.

### Build note (nested-comment build failure, fixed before first green run)

- The mailbox test's first `west twister` attempt failed to build:
  `error: "/*" within comment [-Werror=comment]` at
  `mailbox_types.h:27:66` and `:28:30`. Root cause: a Doxygen comment's
  prose referenced glob-style paths (`include/aliro/user_device/*`,
  `stack/src/user_device/*`), and the literal two-character sequence
  `/*` inside that prose was read by the compiler as an attempted nested
  comment start. Fixed by rewording the prose to avoid the `/*`
  character sequence entirely (no code or behavior change).

### On-target bug found and fixed (shell thread stack overflow)

- Once the DK became available, `aliro-ud credential commit` (the first
  time in this application's history that a full staging transaction was
  driven interactively over the DK's real shell UART — every prior AWP's
  DK evidence exercised `info`/`auth`/read-only `credential` commands or
  a *test binary's* `ztest` output, never a live `commit` typed over the
  shell) crashed the target:
  ```
  ***** USAGE FAULT *****
    Stack overflow (context area not valid)
  >>> ZEPHYR FATAL ERROR 2: Stack overflow on CPU 0
  Current thread: 0x20004e08 (shell_uart)
  Halting system
  ```
  Root cause: `CONFIG_SHELL_STACK_SIZE` had never been raised from
  Zephyr's 2048 B default. `credential commit`'s call chain builds a full
  `PersistedCredential` (two embedded 1024 B `OptionalDocument` buffers
  plus a 16-entry `Binding` array, roughly 3 KB) on the shell thread's own
  stack before it is copied into the committed store — the same class of
  issue as AWP5's `CONFIG_ZTEST_STACK_SIZE` fix
  (`tests/.../crypto/prj.conf`, 8192 -> 16384), but on the *interactive
  shell* thread instead of a `ztest` thread, and undetected until now
  because no earlier AWP's DK session ever ran a real `commit`. This is a
  pre-existing AWP3 gap, not something newly introduced by AWP6's own
  code (mailbox `Store`/`Sessions` are not on `commit`'s call path).
  Fixed by adding `CONFIG_SHELL_STACK_SIZE=16384` to
  `applications/aliro-nfc-user-device/prj.conf`. An intermediate value of
  8192 was tried first and still overflowed (same fault, different
  register values); 16384 (the same target AWP5 landed on for the
  equivalent host-test problem) succeeded and left every subsequent
  command in the sequence below fully working.

### Commands run and results

Toolchain invoked via `ncs4`, from `/home/mak5-local/gesture-access` (west
topdir).

1. `west twister -T ncs-door-lock-and-access-control.git/tests/functional/subsys/aliro_nfc_user_device -p native_sim/native/64`
   — **Result: pass.** 7 of 7 test configurations, 86/86 test cases
   (`host_smoke`, `apdu_fragment_assembler`, `mailbox` [26 — new],
   `authorization` [17], `worker_lifecycle` [12], `cli_info` [6 — one new
   mailbox case added], `crypto` [17]).
2. `west build -p -b nrf54lm20dk/nrf54lm20a/cpuapp -d /tmp/build-aliro-nfc-user-device ncs-door-lock-and-access-control.git/applications/aliro-nfc-user-device`
   — **Result: pass.** Final FLASH 159640 B (7.66%), RAM 90344 B (17.27%)
   with `CONFIG_SHELL_STACK_SIZE=16384` (up from AWP5's 156616 B/82120 B
   baseline — the mailbox module's per-credential committed byte storage
   and session tables, plus the larger shell stack, account for the
   increase).
3. `west build -p -b nrf54lm20dk/nrf54lm20a/cpuapp -d /tmp/build-mailbox-dk ncs-door-lock-and-access-control.git/tests/functional/subsys/aliro_nfc_user_device/mailbox`, then `west flash`, then read the shell UART (`/dev/ttyACM1`, 115200-8N1) across a board reset (physical DK, serial `1051885995`, same board as prior AWPs) — **Result: pass, on real hardware.** All 26 host test cases (`aliro_ud_mailbox_store` [10], `aliro_ud_mailbox_sessions` [12], `aliro_ud_mailbox_fault_injection` [4]) passed unchanged on-target.
4. `west build -p -b nrf54lm20dk/nrf54lm20a/cpuapp -d /tmp/build-aliro-nfc-user-device ncs-door-lock-and-access-control.git/applications/aliro-nfc-user-device`, `west flash`, then drove the real interactive `aliro-ud` shell over `/dev/ttyACM1` (115200-8N1, same DK) with a Python/`pyserial` script — **Result: pass, on real hardware**, after fixing the `CONFIG_SHELL_STACK_SIZE` bug above. Full sequence and observed output:
   - `credential begin-create` / `set-key` / `set-policy` / `set-binding`
     / `set-mailbox 8 3` / `commit` -> `OK handle=1`.
   - `mailbox inspect 1` -> `OK handle=1 size=8 readable=1 writable=1
     settable_in_auth1=0 initialized=0 has_data=0` (config visible
     immediately after commit, not yet initialized).
   - `mailbox init 1` -> `OK`; `mailbox inspect 1` ->
     `...initialized=1 has_data=0`; `mailbox read 1 0 8` ->
     `OK data=0000000000000000`.
   - `mailbox reset 1` -> `OK`; `mailbox read 1 0 8` ->
     `OK data=0000000000000000` (re-zero confirmed).
   - Power-cycle/reset persistence: reset the DK
     (`nrfutil device reset --reset-kind RESET_SYSTEM`), then
     `credential list` -> `OK handle=1 ... has_mailbox=1`, `mailbox
     inspect 1` -> `...initialized=1 has_data=0`, `mailbox read 1 0 8` ->
     `OK data=0000000000000000`: both the credential and its mailbox's
     initialized/zeroed state survived a real reset, confirming
     `ALIRO-UD-SYRS-P1-032`'s "retain ... across reset" requirement
     end-to-end (Zephyr settings/NVS-backed, real flash).
   - `credential delete 1` -> `OK`; `credential list` -> `OK count=0`;
     `mailbox inspect 1` -> `ERR 4 command=mailbox inspect`: confirms the
     mailbox is erased and its handle no longer resolves once the owning
     credential is deleted (privacy/no-stale-data requirement).
   - No physical NFC reader was available in this environment, so the
     authorized-NFC-EXCHANGE portion of `APP_PLAN.md` AWP6's verify item
     ("demonstrate authorized NFC read/write/set plus rollback on a
     failed EXCHANGE") remains unperformed — see Outstanding items.

### Verification-method mapping (SyRS codes: T/D/I/A)

- `ALIRO-UD-SYRS-P1-032` through `-034` (application/backend portions) —
  **T**: the new `mailbox` host suite (26 cases) directly exercises
  persistent per-credential storage with provisioned size/permissions,
  bounds-checked read/write/set, atomic commit, rollback, and
  fault-injected/simulated-reboot persistence. Updated to
  `app-implemented`; EXCHANGE parsing, permission orchestration at the
  wire level, and secure-channel success/error sequencing remain
  stack-owned/WP6-pending, per `APP_PLAN.md`'s own AWP6 scope note. **D**:
  the same 26 host cases re-run unchanged on a physical nRF54LM20 DK
  (item 3 above), plus a full interactive DK session (item 4) provisioning
  a real credential+mailbox over the shell UART, inspecting/initializing/
  reading/resetting it, confirming both the credential and its mailbox
  survive a real board reset, and confirming `credential delete` erases
  the mailbox. Authorized NFC EXCHANGE read/write/set/rollback is not yet
  demonstrated (no reader in this environment; see Outstanding items).
- `ALIRO-UD-SYRS-P1-035`/`-036` (secure-channel error/success sequencing)
  remain `not-yet-verifiable`: those sequences are entirely stack-owned
  and this AWP has no application-side deliverable against them beyond
  `Mailbox`'s own `AliroError` return codes, which the stack would need
  to translate into wire status words.
- `ALIRO-UD-SYRS-P1-008` (Kpersistent/mailbox non-export) — the mailbox
  portion is now `app-implemented`: mailbox data is application-owned,
  non-secret plaintext (never a key), so no export/logging risk exists
  for it as such; `Kpersistent` generation/derivation itself is still
  unimplemented (no stack orchestration calls it yet) and remains a gap.

### External stack observations (not blocking AWP6)

- Confirmed (see "Public contract inspected" above): no code anywhere in
  the checked-out `ncs-aliro` stack currently calls
  `Aliro::Interface::UserDevice::Mailbox`. This AWP's session engine is
  therefore exercised only by this AWP's own host tests and the CLI's
  Credential-Issuer-level `Store` layer, not by any real AUTH0/EXCHANGE
  flow — consistent with `APP_PLAN.md` AWP6's own "Validate independently
  if the current stack does not yet exercise mailboxes" instruction.
- The 36 KiB `storage_partition` size referenced in earlier AWPs'
  planning was not re-derived from the board's devicetree in this pass;
  `mailbox_store.cpp` adds only a lightweight compile-time assertion
  (`kMaxCredentials * kMaxSizeBytes` against a conservative bound) rather
  than a devicetree-driven partition-fit check. Flagged as a known gap,
  not fixed here — the same caveat already applies to AWP3's credential
  storage sizing.

### Security check

- `grep`-reviewed every new/changed file: mailbox bytes are
  application-defined payload, never a key or credential secret, so no
  export/logging restriction applies to their *content*; `mailbox read`
  is a deliberate, explicit Credential-Issuer-level CLI command (not a
  Reader-facing path) and is documented as such in `cli.cpp`. No
  `MailboxRecord`/session shadow-buffer byte is logged anywhere.

### Outstanding items

- No physical NFC reader was available in this environment, so
  `APP_PLAN.md` AWP6's "demonstrate authorized NFC read/write/set plus
  rollback on a failed EXCHANGE" verify item remains unperformed; the
  `Aliro::Interface::UserDevice::Mailbox` session engine itself is fully
  covered by the host suite (isolation/staging/commit/rollback), but only
  the Credential-Issuer-level `Store` path (CLI) has been exercised
  on-target, not a real Reader-driven session.
- `ALIRO-UD-SYRS-P1-035`/`-036` remain `not-yet-verifiable`, unchanged —
  no application-side deliverable exists for them yet (see mapping
  above).
- `west-aliro.yml` and `docs/Requirements.pdf` remain untracked/unstaged
  per explicit user instruction; not committed by this pass.

## AWP7 — Timing and resource instrumentation

### Baseline

- West topdir: `/home/mak5-local/gesture-access` (manifest file
  `west-aliro.yml`).
- `ncs-aliro` checked-out revision at the start of this AWP:
  `1af821adc22e04545eea1ec8bb21cec743780ffa` ("user_device: WP6: complete
  expedited crypto boundary and pre-WP7 fixes"). Unchanged by this AWP
  (no `west update` was run). `west-aliro.yml` on disk points at a
  personal dev fork/branch (`frun36`/`user-device-dev`), not the pinned
  upstream commit recorded in git history; per explicit user instruction
  this file is not committed, and this AWP does not change it.
- NCS `v3.4.0-99553055607b`, Zephyr `v4.4.0-bf801e4e3d19` (from DK boot
  banner).
- Physical DK available throughout this AWP (`nrfutil device list`:
  serial `1051885995`, same board as every prior AWP).

### Public contract inspected

- `include/aliro/user_device/interface.h` —
  `Aliro::Interface::UserDevice::Nfc::TimingConstraints` (a single
  `mMaxResponseTimeMs` field) and `GetTimingConstraints(ConnectionHandle)`.
  Confirmed by grep that the checked-out `ncs-aliro` stack itself never
  calls `GetTimingConstraints()` anywhere (`stack/src/user_device/*`);
  the only other implementation in the whole workspace is
  `ncs-aliro/tests/user_device/dut_adapter/fake_nfc.cpp`, which also
  returns a default-constructed `TimingConstraints{}`. This is a
  descriptive/informational hook the application may fill in, not a
  bound the stack itself enforces or consults.
  `ncs-aliro/docs/user_device_api_design_record.md` §7 independently notes
  this is a placeholder single-field shape that may be extended once
  `ALIRO-UD-SYRS-P1-040` timing bounds are mapped to specific APDU
  exchanges — i.e. by design, not yet.

### Timing-bound research (Deliver item: "query the Aliro test plan for the
exact timing bounds applicable to the selected PICS")

- Searched the Aliro 1.0 Specification and Test Plan corpus (both PDFs)
  for a normative, numeric NFC command-processing-time bound. Found none
  applicable to this application's PICS (NFC-only, Expedited phase,
  Access Credential provisioning/mailbox/authorization). The only
  explicit application-layer timeout found anywhere in the corpus is a
  1500 ms value that applies to the *BLE* interface, which this
  application does not implement (NFC-only PICS; `APP_PLAN.md`).
- The transport-level ISO-DEP Frame Waiting Time (`nrf/subsys/nfc/t4t/isodep.c`,
  citing NFC Forum Digital Protocol Technical Specification §14.8.1) is
  the closest thing to a "protocol timing bound" this application is
  subject to, but: (a) it is negotiated and enforced entirely inside
  `nfc_t4t_lib` itself, never surfaced to `Aliro::Interface::UserDevice::Nfc`
  or this application; (b) `nfc_t4t_lib` exposes no API for the tag
  (listener) role to request a Waiting-Time-Extension itself if it needs
  more of that budget, only to *answer* a WTX request the polling Reader
  chooses to send; and (c) which concrete FWI/FWT value is actually
  negotiated for the listener role's ATS was not conclusively established
  from the library source in the time available for this AWP (the ATS/TB
  parsing code found is written from the poller-role perspective).
  Given (a)-(c), `GetTimingConstraints()` continues to return
  `TimingConstraints{}` (see `nfc_transport.cpp`'s updated comment)
  rather than assert an unverified numeric bound; this is a documented
  gap, not a silent omission.
- Conclusion: no applicable selected-PICS bound exists for this
  application to measure margin against. Per `APP_PLAN.md` AWP7's own
  Deliver item, the instrumentation below is built and reported as
  informational/regression evidence for `ALIRO-UD-SYRS-P1-040` instead of
  a pass/fail margin-to-bound report.

### Deliverables completed

- `src/platform/nfc/command_timing.h`/`.cpp` — a small, Kconfig-gated
  `CommandTiming` class (`Begin(nowMs)`/`End(nowMs)`, sample count,
  last/max duration, `ResetStats()`) plus free functions
  (`BeginCommandTiming()`/`EndCommandTiming()`/`GetCommandTimingSnapshot()`/
  `ResetCommandTimingStats()`) that wrap it as worker-thread-only global
  state. `Begin()`/`End()` take an explicit `nowMs` (not read from a clock
  internally) so the class is host-testable with a fake/synthetic clock,
  including 32-bit wraparound, without any Zephyr dependency.
- `src/platform/nfc/nfc_worker.cpp` — `BeginCommandTiming()`/
  `EndCommandTiming()` now wrap the single call into
  `Aliro::UserDeviceStack::Instance().HandleCommandApdu()` in
  `HandleEvent()`'s `CommandApdu` case. That call only returns after the
  stack has already invoked
  `Aliro::Interface::UserDevice::Nfc::SendResponseApdu()` synchronously
  (see `nfc_transport.cpp`), so this measures exactly the
  "application boundary from command delivery to response send" the
  Deliver item asks for, without any change to the response-transmission
  path (`nfc_transport.cpp`) itself.
- `src/platform/nfc/Kconfig` — `CONFIG_ALIRO_UD_TIMING_INSTRUMENTATION`
  (bool, default `y`). `src/platform/nfc/CMakeLists.txt` uses
  `target_sources_ifdef()` so `command_timing.cpp` is not even compiled
  into the build when this option is disabled ("removable from
  production builds").
- `src/cli/cli.cpp` — `aliro-ud timing stats` (`OK enabled=<0|1>
  samples=<n> last_ms=<n> max_ms=<n>`) and `aliro-ud timing reset`,
  following the exact same deterministic "OK"/"ERR" line convention as
  every other `aliro-ud` command.
- `tests/functional/subsys/aliro_nfc_user_device/command_timing/` — new
  Twister test with two scenarios:
  - `aliro_nfc_user_device.functional.command_timing` (instrumentation
    enabled, the default): 11 cases — 6 pure logic cases directly
    against the `CommandTiming` class (`Begin`/`End` sequencing, `End()`
    with no pending `Begin()` is a no-op, max-tracking never shrinks,
    `ResetStats()` does not cancel a pending `Begin()`, 32-bit-wraparound
    duration arithmetic, a duplicate `Begin()` overwrites the pending
    start) plus 5 integration cases through the real worker
    thread/queue/`Aliro::UserDeviceStack` and a fake NFC transport
    (mirroring `worker_lifecycle`'s pattern): the enabled snapshot flag,
    one handled command recording exactly one sample, three handled
    commands recording three samples with a monotone max, `reset`
    clearing the snapshot, and a rejected (no-session) APDU never
    recording a sample.
  - `aliro_nfc_user_device.functional.command_timing_disabled`
    (`extra_configs: CONFIG_ALIRO_UD_TIMING_INSTRUMENTATION=n`): the same
    5 integration cases (the 6 pure logic cases are `#if`-excluded, since
    `CommandTiming`'s only implementation, `command_timing.cpp`, is not
    even compiled into this build) — every assertion instead confirms
    `mEnabled=false` and every counter stays `0` regardless of how much
    NFC traffic runs, i.e. a true no-op.
  - This test links the real `platform/crypto/*.cpp` (AWP5) backend, not
    just the credential/authorization backends `worker_lifecycle` links:
    the currently checked-out `ncs-aliro` dev revision's session lifecycle
    (`SessionCrypto` construction/destruction) unconditionally references
    `Aliro::Interface::UserDevice::Crypto`/`CredentialSigning` even for
    this test's SELECT-only traffic (see "External stack observations"
    below); `worker_lifecycle`/`authorization`/`cli_info` do not link it
    and are currently blocked for an unrelated, pre-existing reason.
- `worker_lifecycle`/`authorization`/`cli_info`'s `CMakeLists.txt` — added
  `target_sources_ifdef(CONFIG_ALIRO_UD_TIMING_INSTRUMENTATION ...
  command_timing.cpp)`, mirroring the application's own `CMakeLists.txt`,
  since all three now build the modified `nfc_worker.cpp` and therefore
  need `BeginCommandTiming()`/`EndCommandTiming()` (and, for `cli_info`,
  the CLI's timing commands) to resolve at link time.
- `nfc_transport.cpp`'s `GetTimingConstraints()` — comment rewritten to
  record the timing-bound research above (still returns
  `TimingConstraints{}`; see that section for why).

### Commands run and results

Toolchain invoked via `ncs4`, from `/home/mak5-local/gesture-access` (west
topdir).

1. `west twister -T ncs-door-lock-and-access-control.git/tests/functional/subsys/aliro_nfc_user_device/command_timing -p native_sim/native/64`
   — **Result: pass.** 2 of 2 test configurations, 16 of 16 test cases
   (11 with instrumentation enabled, 5 with it disabled; see
   "Deliverables completed" above for the exact case list/split).
2. `west build -p -b nrf54lm20dk/nrf54lm20a/cpuapp -d /tmp/build-aliro-nfc-user-device ncs-door-lock-and-access-control.git/applications/aliro-nfc-user-device`
   — **Result: pass.** Resource report (Deliver item: "report flash and
   RAM use from the build"), all three built against the same DK target
   and the AWP6 baseline's committed tree (`git stash`, rebuilt, then
   restored):
   | Build | FLASH | RAM |
   |---|---|---|
   | AWP6 baseline (this AWP's own changes stashed out) | 181028 B (8.68%) | 119048 B (22.75%) |
   | This AWP, `CONFIG_ALIRO_UD_TIMING_INSTRUMENTATION=n` | 181376 B (8.70%) | 119048 B (22.75%) |
   | This AWP, `CONFIG_ALIRO_UD_TIMING_INSTRUMENTATION=y` (default) | 181568 B (8.71%) | 119072 B (22.76%) |

   The disabled-vs-baseline delta (+348 B FLASH, +0 B RAM) is the
   `aliro-ud timing` CLI commands and the always-present
   `CommandTimingSnapshot`-returning inline no-ops; the
   enabled-vs-disabled delta (+192 B FLASH, +24 B RAM) is the
   instrumentation itself (`command_timing.cpp`'s `CommandTiming` class
   instance and the two `k_uptime_get_32()` call sites in
   `nfc_worker.cpp`) — confirming the Deliver item's "lightweight" goal.
3. `west flash` (same DK, serial `1051885995`), then drove the real
   interactive `aliro-ud` shell over `/dev/ttyACM1` (115200-8N1) with a
   Python/`pyserial` script — **Result: pass, on real hardware.**
   ```
   aliro-ud info
   OK version=0.2.0-awp2+0 init=running session_active=0 activation_attempts=0 rejected_apdus=0
   aliro-ud timing stats
   OK enabled=1 samples=0 last_ms=0 max_ms=0
   aliro-ud timing reset
   OK
   aliro-ud timing stats
   OK enabled=1 samples=0 last_ms=0 max_ms=0
   ```
   Confirms the CLI is wired correctly and reports `enabled=1` (matching
   the default `CONFIG_ALIRO_UD_TIMING_INSTRUMENTATION=y`) with an
   all-zero snapshot before any NFC command has been handled. No
   physical NFC reader was available in this environment (unchanged from
   AWP6), so no real on-target command-to-response sample was collected;
   see Outstanding items.

### Verification-method mapping (SyRS codes: T/D/I/A)

- `ALIRO-UD-SYRS-P1-040` — **T**: `command_timing`'s 16 host test cases
  cover the `CommandTiming` class's duration/wraparound/reset logic in
  isolation and the real worker-to-response wiring end to end (fake NFC
  transport), plus the disabled-build no-op behavior. **D**: the DK build
  report above (item 2) is the "resource report" half of this
  requirement's evidence; the CLI session (item 3) confirms the
  instrumentation is live and reachable on real hardware. Remains
  `not-yet-verifiable`: no applicable selected-PICS *bound* exists to
  measure margin against (see "Timing-bound research" above), and no
  on-target command-to-response *sample* was collected (no NFC reader
  available) — both required by this requirement's "meet every ...
  bound ... on target DK" wording, which this AWP's evidence cannot yet
  satisfy for either half.

### External stack observations (not blocking AWP7's own deliverables)

- Confirmed (as above): the checked-out `ncs-aliro` stack never calls
  `GetTimingConstraints()`. This AWP's instrumentation is therefore
  entirely application-side/informational, consistent with `APP_PLAN.md`
  AWP7's Deliver item wording ("measure the application boundary", not
  "enforce a stack-provided bound").
- Unrelated pre-existing build breakage found while validating this AWP:
  `worker_lifecycle`, `authorization`, and `cli_info` currently fail to
  *link* against the checked-out `ncs-aliro` dev revision
  (`undefined reference to Aliro::Interface::UserDevice::Crypto::DestroyKey`
  and several sibling `Crypto`/`CredentialSigning` symbols, from
  `UserDeviceStateMachine::DestroyAccessProtocolSession()`/`HandleAuth1()`
  and `SessionCrypto::Destroy()`). Confirmed this is **not** caused by
  this AWP's changes: reverting every file this AWP touches (`git stash`)
  and rebuilding those same three tests against the same checked-out
  `ncs-aliro` revision reproduces the identical link error. Root cause:
  the locally checked-out `ncs-aliro` revision's session-teardown path
  now unconditionally references the `Crypto`/`CredentialSigning`
  interfaces (even for SELECT-only traffic with no AUTH0/AUTH1), which
  these three tests' `CMakeLists.txt`/`Kconfig` predate and never linked.
  Temporarily checking `ncs-aliro` out to the git-history-pinned upstream
  commit (`b8cd7c5fe7c6f50df4a6bb89a750c1a985e26e0d`) to isolate this
  further was inconclusive: that revision's module/Kconfig layout is
  old enough that even `host_smoke`/`mailbox`/`crypto` fail with an
  unrelated `undefined symbol NCS_ALIRO_USER_DEVICE` Kconfig error,
  indicating that commit predates something the application's own
  `Kconfig.defconfig` now depends on and is not a safe like-for-like
  comparison without a full `west update` (not run, to avoid disrupting
  the workspace's other in-progress local state).
  Adding the `platform/crypto/*.cpp` backend to these three tests (as
  done for `command_timing`, see "Deliverables completed") does fix the
  *link* error, but uncovers a second, distinct, also-pre-existing
  runtime bug one call deeper: `worker_lifecycle`'s
  `test_on_timeout_termination_on` then hits
  `ASSERTION FAIL [!arch_is_in_isr()] ... mutexes cannot be used inside ISRs`
  during the stack's own session-watchdog-timeout teardown path (a PSA
  crypto mutex acquired from what native_sim treats as ISR/timer
  context). This was left unfixed and the crypto-backend addition
  reverted for these three tests: both bugs are inside the external
  `ncs-aliro` stack's session-teardown/crypto-destroy path, which
  `APP_PLAN.md`'s boundary rules place out of scope for this application
  to modify, and neither is caused by or related to this AWP's own
  timing-instrumentation changes.
- Given the above, `worker_lifecycle`/`authorization`/`cli_info` are left
  exactly as this AWP found them (build-broken against the currently
  checked-out dev revision), plus the one `command_timing.cpp` linkage
  addition every one of them now structurally needs regardless (see
  "Deliverables completed"). `command_timing`, `host_smoke`, `mailbox`,
  and `apdu_fragment_assembler` all remain green.

### Security check

- `command_timing.h`/`.cpp` records only integer durations/counts derived
  from `k_uptime_get_32()`; no APDU content, key material, or credential
  data is ever read, stored, or logged by this module.
- `aliro-ud timing stats`/`reset` follow the same non-secret,
  deterministic "OK"/"ERR" line convention as every other `aliro-ud`
  command; nothing new is added to the shell's attack surface beyond two
  more read/reset-only diagnostic commands.

### Outstanding items

- No physical NFC reader was available in this environment (unchanged
  from every prior AWP that noted this), so no real on-target
  command-to-response timing sample was collected; `aliro-ud timing
  stats` was confirmed live and reachable (all-zero snapshot) but not
  exercised against real NFC traffic. Collecting one is a pure hardware
  dependency, not an application gap.
- No normative, numeric NFC processing-time bound applicable to this
  application's selected PICS was found anywhere in the Aliro 1.0
  Specification and Test Plan corpus (see "Timing-bound research"
  above); `ALIRO-UD-SYRS-P1-040` therefore cannot be marked
  `verified-end-to-end` by this or any future AWP unless such a bound is
  identified elsewhere (e.g. a future spec revision, or explicit
  clarification), or the requirement's own wording is revisited.
- The pre-existing `worker_lifecycle`/`authorization`/`cli_info` link
  breakage and the deeper watchdog-teardown ISR-mutex bug (see "External
  stack observations") remain unfixed: both are inside the external
  `ncs-aliro` stack, out of scope for this application per `APP_PLAN.md`'s
  boundary rules, and were not introduced by this AWP.
- `GetTimingConstraints()` continues to return `TimingConstraints{}`;
  see "Timing-bound research" for the specific gaps (listener-role
  FWI/FWT value not conclusively established; no WTX-request API exposed
  to the tag role) that would need to be closed before it could return a
  meaningful non-zero value.
- `west-aliro.yml` and `docs/Requirements.pdf` remain untracked/unstaged
  per explicit user instruction; not committed by this pass.
