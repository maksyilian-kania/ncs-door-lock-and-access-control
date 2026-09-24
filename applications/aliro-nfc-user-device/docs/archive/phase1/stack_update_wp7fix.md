# Stack update: `ncs-aliro` WP7-FIX1..FIX6 re-verification

**Not a numbered AWP.** `APP_PLAN.md` §3's AWP8 was the final numbered
package and is already committed (`1204d10`). This is the "After any stack
update" procedure from `APP_PLAN.md` §4, run because the checked-out
`ncs-aliro` advanced past the revision AWP8's evidence recorded, before
attempting a live reader-tap session per the user's request.

## What changed upstream

- Last revision this application built/tested against (`docs/evidence/AWP8.md`):
  `e5022e21b694a400d6349772185380a1bb5c5e8d` ("WP7-S9: complete-library
  campaign, WP7 traceability closure").
- Checked-out `ncs-aliro` HEAD at the start of this pass:
  `198def4e6a66144049a7fd1f56378b9cbba7a7c0` ("WP7-FIX6: add run record and
  traceability finding row"), a descendant of the minimum baseline
  `b8bed857b482d288168185e76d5452469739fbdd` (confirmed via
  `git -C ncs-aliro log --oneline b8bed857..HEAD`, 29 commits). No
  `west update` was run — this is the revision that was already checked out.
- Ten commits between the two (`e5022e21`..`198def4e`): `WP7-FIX1` (+A1,
  mandatory `0x8C`/state-indicator atomic session), `WP7-FIX2` (defer
  mailbox commit until after response encryption), `WP7-FIX3` (fail closed
  on mailbox metadata resolution failure), `WP7-FIX4` (field-width bound
  and nested unknown-tag rule), `WP7-FIX5` (strengthen three vacuous test
  assertions), `WP7-FIX6` (whitespace/traceability reconciliation), plus
  two unlabeled "Development harness" commits. Per `ncs-aliro`'s own
  `docs/execution/current.md`, this closes out its entire WP7 campaign
  (`WP8`-`WP11` "not started").
- **No public header changed**: `git -C ncs-aliro diff --stat e5022e21..HEAD -- include/`
  is empty. All ten commits are internal to `stack/` (orchestration,
  mailbox engine, HSM state machine) and stack-side tests/docs. This
  application's implemented contract (`Aliro::Interface::UserDevice::*`)
  is unchanged, so no adapter code in this application needed to change.

## Verification performed

Run via `ncs4 west ...` from the west topdir (`/home/mak5-local/gesture-access`).

1. `west twister -T .../tests/functional/subsys/aliro_nfc_user_device -p native_sim/native/64`
   (full host suite) — **identical result to AWP8**: 6 of 9 configurations
   pass (74 of 110 cases: `host_smoke`, `apdu_fragment_assembler`,
   `mailbox`, `command_timing`, `command_timing_disabled`, `crypto`). The
   same 3 configurations (`worker_lifecycle`, `authorization`, `cli_info`)
   still fail to *link* on the identical pre-existing external
   `Aliro::Interface::UserDevice::Crypto::DestroyKey(unsigned int&)` symbol
   gap already documented in `docs/evidence/AWP7.md`/`wp7_stack_impact.md`.
   Not a regression, not newly introduced, out of scope per `APP_PLAN.md`'s
   stack-boundary rule.
2. `west build -p -b nrf54lm20dk/nrf54lm20a/cpuapp -d /tmp/build-aliro-stackupdate ncs-door-lock-and-access-control.git/applications/aliro-nfc-user-device`
   — **pass**. Resource report:

   | FLASH | RAM |
   |---|---|
   | 186708 B (8.96%) | 121280 B (23.18%) |

   FLASH grew by 240 B versus AWP8's 186468 B (8.94%) — attributable to
   `libaliro.a` growth from the WP7-FIX slices' internal orchestration code
   (mailbox commit ordering, mandatory-tag handling); RAM unchanged.
3. **Physical DK regression** (`nrfutil device list` reported two boards
   this session: `1051885995`, the same board every prior AWP used, and a
   new `1051889440` — both `PCA10184`). Flashed the rebuilt image to
   `1051885995` (`west flash -d /tmp/build-aliro-stackupdate --snr 1051885995`
   — pass) and drove the shell UART (`/dev/ttyACM1`, 115200-8N1) via a
   `pyserial` script:
   - `aliro-ud info` → `OK version=0.2.0-awp2+0 init=running
     session_active=0 activation_attempts=0 rejected_apdus=0` (clean boot).
   - `credential list` → `OK count=0` (DK was already in the clean state
     AWP8 left it in).
   - Full provisioning cycle with a freshly generated, valid P-256 key pair
     (never reused/persisted beyond this session) and a random 16-byte
     `reader_group_identifier`: `begin-create` → `set-key` (64 hex chars)
     → `set-policy 3` → `set-binding 0 <32-hex-char group id> direct
     <130-hex-char pubkey>` → `set-mailbox 8 3` → `commit` → `OK handle=1`;
     `credential inspect 1` → `OK handle=1 bindings=1 policy=3 has_trust=1
     has_mailbox=1 ...`; `mailbox init 1` → `OK`; `mailbox inspect 1` →
     `OK handle=1 size=8 readable=1 writable=1 ... initialized=1
     has_data=0`. Exact match to `docs/provisioning.md`.
   - **Cross-reset persistence**: `nrfutil device reset --serial-number
     1051885995 --reset-kind RESET_SYSTEM` (real board reset, not a
     re-flash). After reset: `credential list`/`credential inspect 1`/
     `mailbox inspect 1` reported the identical committed state as before
     the reset — reproducing AWP6/AWP8's finding unchanged on this stack
     revision.
   - **DK left clean**: `credential reset` → `OK`; `credential list` →
     `OK count=0`; `mailbox inspect 1` → `ERR 4 command=mailbox inspect`.

## Conclusion

No application-observable behavior changed. Every check that passed before
this stack update still passes identically; every pre-existing gap
(the three-configuration link failure, the absence of a physical-reader
demonstration) is unchanged. No `docs/traceability.md` row's status changes
as a result of this pass — no new capability became verifiable, because the
public contract this application implements against did not change, and no
NFC-reader-driven transaction was exercised (see the next section).

## Outstanding: attempting a real reader tap

A second DK (`1051889440`) is now attached. This repository's sibling
`applications/aliro-access-control-app` (RFAL-based NFC + door-lock/UWB
simulation) is a candidate to flash as a local Reader counterpart, but its
own build/AWP status has not been reviewed as part of any
`aliro-nfc-user-device` AWP — its readiness (or a genuine third-party
certified Aliro Reader) must be established separately before a tap
attempt. Not yet performed as of this file.
