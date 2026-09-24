# AWP8 — Documentation, acceptance evidence, and final traceability

Part of the per-AWP evidence log (`docs/evidence/`); see `docs/STATE.md` for
the current checkpoint and `../traceability.md` for requirement status.
Previous: `AWP7.md` (plus its post-AWP7 addendum and `../wp7_stack_impact.md`,
the out-of-band fix pass between AWP7 and this AWP). This is the final AWP
in `APP_PLAN.md` §3.

## Baseline

- West topdir: `/home/mak5-local/gesture-access` (manifest file
  `west-aliro.yml`, untracked/unstaged by explicit user instruction, not
  changed by this AWP).
- `ncs-aliro` checked-out revision at the start of this AWP:
  `e5022e21b694a400d6349772185380a1bb5c5e8d` ("WP7-S9: complete-library
  campaign, WP7 traceability closure") — confirmed via
  `git -C ncs-aliro rev-parse HEAD`, unchanged from `docs/evidence/AWP7.md`'s
  addendum. No `west update` was run; this is a descendant of the minimum
  baseline `b8bed857b482d288168185e76d5452469739fbdd` per
  `git -C ncs-aliro log --oneline b8bed857..HEAD`.
- Application repository (`ncs-door-lock-and-access-control.git`) working
  tree before this AWP: `git status --short` showed only `west-aliro.yml`
  modified and `docs/Requirements.pdf` untracked (both pre-existing,
  excluded per explicit user instruction — not touched by this AWP); no
  other uncommitted state.
- A physical DK was not attached at the start of this AWP
  (`nrfutil device list` reported "Supported devices found: 0"); the
  documentation work below was completed and the user was asked how to
  proceed. The user then attached the DK; `nrfutil device list`
  subsequently reported the same board every prior AWP used (serial
  `1051885995`, `PCA10184`), and the interactive DK session in "Commands
  run and results" below was performed against it.

## Deliverables completed

Per `APP_PLAN.md` AWP8's Deliver list — this AWP is documentation/evidence
only; no `src/`, `Kconfig`, `CMakeLists.txt`, or test source changed:

- `README.md` — fully rewritten to describe the implemented application
  (scope, build/flash, CLI command summary, current status) instead of the
  pre-AWP0 System-OFF/wake-on-field POC it previously still described.
- `docs/architecture.md` — fully rewritten to describe the implemented
  module layout (`platform/{nfc,os,crypto,authorization}`,
  `storage/{credential,mailbox}`, `cli`, `lifecycle`) and the
  application/`Aliro::UserDeviceStack` boundary, replacing the POC's
  System-OFF/NFCT-wake architecture description.
- `docs/provisioning.md` (new) — the field-based CLI provisioning workflow
  (staging transaction create/update/commit/abort, mailbox
  inspect/init/read/reset, button authorization, diagnostics) with example
  deterministic `OK`/`ERR` output for every `aliro-ud` command, cross-checked
  against `src/cli/cli.cpp`'s actual output formats (see "Commands run and
  results" below) and containing no real secret values.
- `docs/traceability.md` — added narrative paragraphs for AWP7 (previously
  missing) and this AWP; verified every `ALIRO-UD-SYRS-P1-001` through
  `-040` requirement is represented exactly once as a table row (see
  "Commands run and results," item 4). No status changed: this AWP performs
  no new implementation, and re-running every host/build check reproduced
  the same pass/fail split AWP7's addendum already recorded, so every row's
  status and citation are left exactly as the AWP that produced them set
  them.
- This file (`docs/evidence/AWP8.md`).

## Commands run and results

Toolchain invoked via `ncs4`, from `/home/mak5-local/gesture-access` (west
topdir).

1. `git -C ncs-aliro rev-parse HEAD` → `e5022e21b694a400d6349772185380a1bb5c5e8d`
   (unchanged from AWP7's addendum). `git -C ncs-aliro log --oneline
   b8bed857b482d288168185e76d5452469739fbdd..HEAD | tail -5` confirmed this
   is a descendant of the minimum baseline.
2. `west twister -T ncs-door-lock-and-access-control.git/tests/functional/subsys/aliro_nfc_user_device -p native_sim/native/64`
   (the whole application host-test suite) — **Result: 6 of 9
   configurations pass** (`host_smoke`, `apdu_fragment_assembler`,
   `mailbox`, `command_timing`, `command_timing_disabled`, `crypto`; 74 of
   110 test cases). **3 of 9 configurations still fail to link**
   (`worker_lifecycle`, `authorization`, `cli_info`), on the identical
   pre-existing `undefined reference to
   Aliro::Interface::UserDevice::Crypto::DestroyKey(unsigned int&)` (and
   sibling `Crypto`/`CredentialSigning` symbols) already documented in
   `docs/evidence/AWP7.md`'s "External stack observations" and reconfirmed
   unchanged in `../wp7_stack_impact.md`. This AWP changed no source that
   could affect that link error; re-running the exact same suite reproduces
   the exact same 6-pass/3-link-error split byte-for-byte against the
   command output already on record. Not a regression, not fixed by this
   AWP, out of scope per `APP_PLAN.md`'s stack-boundary rule.
3. `west build -p -b nrf54lm20dk/nrf54lm20a/cpuapp -d /tmp/build-aliro-awp8 ncs-door-lock-and-access-control.git/applications/aliro-nfc-user-device`
   — **Result: pass.** Resource report:

   | FLASH | RAM |
   |---|---|
   | 186468 B (8.94%) | 121280 B (23.18%) |

   Identical to the WP7-stack-impact-fix figures recorded in
   `../wp7_stack_impact.md` and `docs/evidence/AWP7.md`'s addendum,
   confirming no build-affecting drift since that fix.
4. `grep -o "ALIRO-UD-SYRS-P1-[0-9]\{3\}" traceability.md | ...` /
   `grep "^| ALIRO-UD-SYRS-P1-" traceability.md | ... | uniq -c` — confirmed
   all 40 Phase 1 requirements (`-001` through `-040`) appear as exactly one
   table row each, with no duplicate and no gap.
5. Documented commands in `docs/provisioning.md` were cross-checked
   line-by-line against `src/cli/cli.cpp`'s `shell_print()` format strings
   (`CmdCredentialCommit`, `CmdCredentialInspect`/`PrintMetadataLine`,
   `CmdCredentialList`, `CmdCredentialBindings`, `CmdCredentialPreferredSet`/
   `Get`, `CmdMailboxInspect`, `CmdMailboxRead`, `CmdMailboxInit`,
   `CmdMailboxReset`, `CmdTimingStats`/`Reset`, `PrintError`) rather than
   reproduced from memory, and against the real example output already
   captured on a physical DK in `docs/evidence/AWP2.md`, `AWP4.md`, `AWP6.md`,
   and `AWP7.md` (where such a session exists). No new DK session was run
   for this AWP's own documentation pass (see "Outstanding items").
6. `git status --short` re-checked after all edits above: only this AWP's
   four documentation files are new/modified, plus the pre-existing
   `west-aliro.yml`/`docs/Requirements.pdf` exclusions untouched.
7. **DK became available mid-AWP.** `nrfutil device list` →
   `1051885995`/`PCA10184` (same board as every prior AWP). `west flash -d
   /tmp/build-aliro-awp8` — **Result: pass** ("Board(s) with serial
   number(s) 1051885995 flashed successfully"). Drove a `pyserial` script
   against the real shell UART (`/dev/ttyACM1`, 115200-8N1) reproducing
   every command sequence documented in `../provisioning.md`, using
   placeholder key/binding hex (`AA`/`BB` repeated; never real material):
   - Section 2/3 (stage → commit → inspect → bindings): `begin-create` →
     `set-key`/`set-policy 3`/`set-binding 0 <32-hex-char group id> direct
     <130-hex-char pubkey>`/`set-mailbox 8 3` → `commit` → `OK handle=1`;
     `credential list`/`inspect 1` → `OK handle=1 bindings=1 policy=3
     has_trust=1 has_mailbox=1 has_credential_timestamp=0
     has_revocation_timestamp=0`; `credential bindings 1` → `OK count=1
     bindings=00112233445566778899aabbccddeeff`. **Exact match** to
     `../provisioning.md`'s documented output (after fixing this file's
     own initially-wrong example hex lengths — see below).
   - Section 4 (mailbox): `mailbox inspect 1` → `initialized=0`; `mailbox
     init 1` → `OK`; re-`inspect` → `initialized=1`; `mailbox read 1 0 8`
     → `OK data=0000000000000000`. Exact match.
   - Preferred-credential selection: `preferred-set
     00112233445566778899AABBCCDDEEFF 1` → `OK`; `preferred-get` (same
     identifier) → `OK handle=1`. Exact match.
   - Section 5 (delete): `credential delete 1` → `OK`; `credential list`
     → `OK count=0`; `mailbox inspect 1` → `ERR 4 command=mailbox
     inspect`. Exact match.
   - Section 6 (authorization): `auth status` (initial) → `OK
     state=required remaining_ms=0`; `auth press` (test trigger) → `OK`;
     `auth status` → `OK state=authorized remaining_ms=29600`; `auth
     notify-required` → `OK` plus a real
     `aliro_ud_authorization: Authorization required ...; no valid button
     window, indicating` log line; `auth clear` → `OK`; `auth status` →
     `OK state=required remaining_ms=0`. Exact match.
   - Diagnostics: `timing stats` → `OK enabled=1 samples=0 last_ms=0
     max_ms=0` (no NFC reader available to generate a real sample, same
     gap as AWP7/AWP7-addendum).
   - **Cross-reset persistence**: provisioned a second credential+mailbox
     (`set-key`/`set-policy 2`/`set-mailbox 8 3` → `commit` → `OK
     handle=1`; `mailbox init 1` → `OK`), then `nrfutil device reset
     --serial-number 1051885995 --reset-kind RESET_SYSTEM` (a real board
     reset, not a re-flash). After the reset: `aliro-ud info` reported
     `init=running` (clean reboot); `credential list` and `mailbox
     inspect 1` reported the identical committed state as before the
     reset (`policy=2 has_mailbox=1`, `initialized=1 has_data=0`) —
     reproducing AWP6's cross-reset finding on this AWP's own tree.
   - **DK left clean**: `credential reset` → `OK`; `credential list` →
     `OK count=0`; `mailbox inspect 1` → `ERR 4 ...`, confirming no
     provisioned state was left behind for a future session.
   - One documentation defect found and fixed by this exercise:
     `../provisioning.md`'s example `set-binding`/`preferred-set`/`get`
     hex values were initially too short (a placeholder
     `reader_group_identifier` shorter than 16 bytes, a placeholder public
     key shorter than 65 bytes with the wrong prefix byte) and returned
     `ERR INVALID_ARGUMENT` on real hardware instead of `OK`. Corrected in
     `../provisioning.md` (added an explicit length/prefix note:
     `reader_group_identifier` is exactly 32 hex chars/16 bytes,
     `Aliro::UserDevice::kReaderGroupIdentifierLength`; the public key is
     exactly 130 hex chars/65 bytes with a `0x04` prefix,
     `Aliro::CryptoTypes::kEccP256PublicKeyLength`) and re-verified against
     the DK session above before this AWP's commit.

## Verification-method mapping (SyRS codes: T/D/I/A)

This AWP performs no *new* application behavior, so no row's status or
verification-method mapping changed — see `../traceability.md` for the
per-row detail, which already cites the exact AWP and evidence file that
produced each row's current status. This AWP's own verification is: **I**
(inspection) over every requirement row's existing evidence; **A**
(analysis) confirming the table's completeness (item 4 above); and **D**
(demonstration) re-running the documented CLI workflow on the physical DK
(item 7 above) to confirm `../provisioning.md` matches real behavior
byte-for-byte (which also caught and fixed one documentation-only defect,
not an application defect — see item 7's last bullet).

## Security check

- `docs/provisioning.md`'s example command-line arguments use clearly
  fake placeholder hex (`AA` repetitions, sequential bytes, angle-bracket
  parameter names) for every field a real invocation would fill with a
  private-key scalar, binding key, or timestamp; no real key or credential
  material appears in any file this AWP touched.
- Re-confirmed (by inspection, not a new grep pass beyond item 5 above)
  that no committed evidence file, this AWP's new documentation, or
  `traceability.md` contains private key, `Kpersistent`, or session-key
  bytes.

## External stack observations (not caused by this AWP)

- `worker_lifecycle`/`authorization`/`cli_info`'s pre-existing link failure
  against the checked-out `ncs-aliro` dev revision (see item 2 above) is
  unchanged since AWP7/the WP7-stack-impact fix pass. Still out of scope
  for this application to fix per `APP_PLAN.md`'s boundary rules.
- No new external stack observations were made during this AWP: no source
  under `src/` or `tests/` was modified, so no new build/link/runtime
  interaction with the checked-out stack was exercised beyond re-running
  the exact same suite already characterized in `docs/evidence/AWP7.md`
  and `../wp7_stack_impact.md`.

## Outstanding items

- The DK-availability blocker recorded earlier in this AWP was resolved:
  the user attached a physical DK mid-invocation (see "Commands run and
  results" item 7), so `../provisioning.md`'s documented CLI workflow was
  re-verified live rather than by citation alone. This AWP's documentation
  changes are committed accordingly.
- No new host-testable behavior was added by this AWP, so there is no new
  gap beyond the ones already carried forward and unchanged since AWP7:
  the `worker_lifecycle`/`authorization`/`cli_info` link failure (external
  stack, item 2 above), the absence of any applicable numeric NFC timing
  bound for `ALIRO-UD-SYRS-P1-040` (`docs/evidence/AWP7.md`), and the
  absence of any physical NFC reader in every development environment used
  by any AWP to date (affects every `not-yet-verifiable` row in
  `../traceability.md` that depends on a real `AUTH0`/`AUTH1`/`EXCHANGE`
  transaction).
- `west-aliro.yml` and `docs/Requirements.pdf` remain untracked/unstaged
  per explicit user instruction; not committed by this pass.
