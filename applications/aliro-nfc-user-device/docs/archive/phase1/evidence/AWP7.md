# AWP7 — Timing and resource instrumentation

Part of the per-AWP evidence log (`docs/evidence/`); see `docs/STATE.md` for
the current checkpoint and `../traceability.md` for requirement status.
Previous: `AWP6.md`. Next: AWP8 (not started; see `docs/STATE.md`).

## Baseline

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

## Public contract inspected

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

## Timing-bound research

(Deliver item: "query the Aliro test plan for the exact timing bounds
applicable to the selected PICS")

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

## Deliverables completed

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

## Commands run and results

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

## Verification-method mapping (SyRS codes: T/D/I/A)

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

## External stack observations (not blocking AWP7's own deliverables)

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

## Security check

- `command_timing.h`/`.cpp` records only integer durations/counts derived
  from `k_uptime_get_32()`; no APDU content, key material, or credential
  data is ever read, stored, or logged by this module.
- `aliro-ud timing stats`/`reset` follow the same non-secret,
  deterministic "OK"/"ERR" line convention as every other `aliro-ud`
  command; nothing new is added to the shell's attack surface beyond two
  more read/reset-only diagnostic commands.

## Outstanding items

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

## Addendum: WP7 stack impact remediation (post-AWP7, before AWP8)

Between this AWP and the start of AWP8, the checked-out `ncs-aliro` stack
advanced through its own WP7 (mailbox/EXCHANGE) work packages and broke the
public `Mailbox` contract AWP3/AWP6 had already implemented against. This
was a separate, out-of-band fix pass, not a numbered AWP; the full
breaking-change analysis, code fix, and its own verification live entirely
in `../wp7_stack_impact.md` (not duplicated here). Summary for this log's
purposes:

- Tested `ncs-aliro` revision after the fix:
  `e5022e21b694a400d6349772185380a1bb5c5e8d` ("WP7-S9: complete-library
  campaign, WP7 traceability closure").
- `mailbox` host suite: 28/28 pass (up from AWP6's 26, reflecting the
  contract change). `command_timing`/`host_smoke`/`apdu_fragment_assembler`
  unaffected/pass. DK build: pass (FLASH 186468 B / 8.94%, RAM 121280 B /
  23.18%).
- `worker_lifecycle`/`authorization`/`cli_info` still fail to *link* in
  `native_sim`, but on the same pre-existing, unrelated `Crypto`/
  `CredentialSigning` symbols already documented in this file's "External
  stack observations" section above, not on anything from this fix.
- See `../wp7_stack_impact.md` for the itemized breaking changes
  (`Mailbox::StageSet()`'s new offset/length/fill-byte signature,
  `MailboxPermissions::mSettableInAuth1`'s removal, five newly-required
  `Mailbox` functions, the new `mailbox_data_subset` provisioning surface,
  and the provisioning wire-format version bump) and the files changed to
  fix them.

Next work: AWP8 ("Documentation, acceptance evidence, and final
traceability"), not yet started. See `docs/STATE.md` for the current
checkpoint.
