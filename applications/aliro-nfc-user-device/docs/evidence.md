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
