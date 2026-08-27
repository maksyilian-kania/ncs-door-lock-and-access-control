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
