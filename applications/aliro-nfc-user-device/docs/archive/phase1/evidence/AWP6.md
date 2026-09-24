# AWP6 — Mailbox persistence

Part of the per-AWP evidence log (`docs/evidence/`); see `docs/STATE.md` for
the current checkpoint and `../traceability.md` for requirement status.
Previous: `AWP5.md`. Next: `AWP7.md`.

> **Post-AWP6/AWP7 note:** the checked-out `ncs-aliro` stack later advanced
> through its own WP7 (mailbox/EXCHANGE) work packages and broke the public
> `Mailbox` contract this AWP implemented against (`StageSet()`'s signature,
> `MailboxPermissions::mSettableInAuth1`'s removal). See
> `../wp7_stack_impact.md` for the full breaking-change analysis and fix;
> the mailbox design described below (session engine, `Store` layer,
> CLI commands) is otherwise still accurate.

## Baseline

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

## Public contract inspected

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

## Deliverables completed

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
  the full mailbox length in one call and is also gated on `mWritable` —
  note: re-shaped by the post-AWP7 WP7-stack-impact fix, see the note at
  top of this file), `Commit` (applies the shadow buffer's dirty bytes to
  committed storage via `Store::ApplyDirtyBytes()` in one call — atomic
  from the caller's perspective — then closes the session; on persistence
  failure, no staged changes are applied and the session is left open so
  the caller may retry), `Rollback`/`Close` (discard staged changes,
  committed bytes unchanged), `Read`/`StageWrite`/`StageSet` all reject
  once the mailbox's *current* provisioned size has shrunk below an
  in-flight session's original bounds (re-validated against
  `Store::GetConfig()` on every call, not cached at `OpenSnapshot()` time).
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

## Build note (nested-comment build failure, fixed before first green run)

- The mailbox test's first `west twister` attempt failed to build:
  `error: "/*" within comment [-Werror=comment]` at
  `mailbox_types.h:27:66` and `:28:30`. Root cause: a Doxygen comment's
  prose referenced glob-style paths (`include/aliro/user_device/*`,
  `stack/src/user_device/*`), and the literal two-character sequence
  `/*` inside that prose was read by the compiler as an attempted nested
  comment start. Fixed by rewording the prose to avoid the `/*`
  character sequence entirely (no code or behavior change).

## On-target bug found and fixed (shell thread stack overflow)

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

## Commands run and results

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

## Verification-method mapping (SyRS codes: T/D/I/A)

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

## External stack observations (not blocking AWP6)

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

## Security check

- `grep`-reviewed every new/changed file: mailbox bytes are
  application-defined payload, never a key or credential secret, so no
  export/logging restriction applies to their *content*; `mailbox read`
  is a deliberate, explicit Credential-Issuer-level CLI command (not a
  Reader-facing path) and is documented as such in `cli.cpp`. No
  `MailboxRecord`/session shadow-buffer byte is logged anywhere.

## Outstanding items

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
- See the note at the top of this file: the `Mailbox` contract described
  above was subsequently broken and fixed by the post-AWP7 WP7-stack-impact
  pass (`../wp7_stack_impact.md`).
