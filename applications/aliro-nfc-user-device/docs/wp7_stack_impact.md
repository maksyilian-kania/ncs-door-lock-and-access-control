# WP7 stack impact remediation

**Read this before starting any further AWP.** This is an out-of-band fix
pass, not a numbered AWP: it exists only to unblock AWP work after the
checked-out `ncs-aliro` stack advanced through its own WP7 (mailbox/EXCHANGE)
work packages and broke the public `Mailbox` contract this application's
AWP6 (and AWP3, for provisioning) already implemented against.

## What happened

- This application's `docs/evidence.md` last recorded a tested `ncs-aliro`
  revision of `1af821adc22e04545eea1ec8bb21cec743780ffa` ("WP6: complete
  expedited crypto boundary and pre-WP7 fixes").
- The checked-out `ncs-aliro` (`west-aliro.yml`, branch `user-device-dev`)
  has since advanced to `e5022e21b694a400d6349772185380a1bb5c5e8d` ("WP7-S9:
  complete-library campaign, WP7 traceability closure") through nine WP7
  slices (`ca28137d`..`e5022e21`).
- Two public headers this application implements against changed:
  `include/aliro/user_device/mailbox.h` and
  `include/aliro/user_device/interface.h` (the `Mailbox` namespace).
  `ncs-aliro`'s own `docs/user_device_api_design_record.md` §"Breaking
  changes" explicitly names this sibling application's `mailbox.cpp`/
  `mailbox_sessions.{h,cpp}` as needing to change.

## Breaking changes and how they were fixed here

1. **`Mailbox::StageSet()` signature change (WP7-S2, amendment A1).**
   - Old: `StageSet(SessionHandle, const uint8_t *data, size_t length)` -
     staged a full-mailbox multi-byte buffer.
   - New: `StageSet(SessionHandle, size_t offset, size_t length, uint8_t
     value)` - fills `[offset, offset + length)` with one repeated byte
     (Table 8-16 `0x95` set request).
   - Fixed in `mailbox_sessions.h`/`.cpp` (implementation re-written to a
     bounds-checked fill loop, mirroring `StageWrite()`'s overflow-safe
     bounds check) and `mailbox.cpp` (adapter forwards the new signature).
   - Host tests updated: `test_mailbox_sessions.cpp`
     (`test_stage_set_fills_range_with_repeated_byte` replaces
     `test_stage_set_requires_exact_full_mailbox_length`; the interface
     adapter test and `test_shrinking_mailbox_mid_session_is_reflected_immediately`
     also updated to the new call shape).

2. **`MailboxPermissions::mSettableInAuth1` removed (WP7-S2, decision D7
   item 8).** The field's premise was wrong: the AUTH1
   `mailbox_data_subset` is read-only from the Reader's perspective (Aliro
   1.0 Specification, section 8.3.1.15, page 61), so there is nothing for a
   Reader to "set". Removed from:
   - The public `Aliro::UserDevice::MailboxPermissions` struct (upstream,
     not touched by this application).
   - This application's own `AliroUd::Credential::MailboxConfig`
     (`credential_types.h`) and `AliroUd::Mailbox::Store::Config`
     (`mailbox_store.h`) - the field no longer maps to anything real, so it
     was deleted here too rather than kept as dead state.
   - The CLI: `credential set-mailbox <size> <rights>`'s `rights` mask is
     now 2 bits (`0x1`=readable, `0x2`=writable); values `> 3` are rejected
     instead of silently accepted. `mailbox inspect`'s
     `settable_in_auth1=<0|1>` output field is gone.

3. **Five new required `Aliro::Interface::UserDevice::Mailbox` functions**
   the stack now actively calls (confirmed by grepping `ncs-aliro`'s
   `stack/src/user_device/mailbox/mailbox_engine.cpp` and
   `stack/src/user_device/hsm/state_machine.cpp` for real call sites, not
   just declarations) - previously entirely unimplemented in this
   application, now implemented in `mailbox.cpp`:
   - `ResolveForCredential(CredentialHandle, MailboxHandle&)` - maps a
     credential to its mailbox handle via `Store::GetConfig()`; returns
     `ALIRO_INVALID_STATE` (not `ALIRO_INVALID_ARGUMENT`) for a credential
     with no provisioned mailbox, matching the documented "observably
     identical to an invalid handle" contract.
   - `GetMetadata(MailboxHandle, MailboxMetadata&)` - size + permissions
     from `Store::GetConfig()`.
   - `HasNonZeroData(MailboxHandle, bool&)` - forwards to
     `Store::HasNonZeroData()` (committed bytes only, per WP7-S1 decision
     D6).
   - `GetMailboxDataSubsetPairCount(MailboxHandle, bool& outConfigured,
     size_t& outCount)` and `GetMailboxDataSubsetPair(MailboxHandle,
     size_t index, uint16_t& outOffset, uint16_t& outLength)` - the AUTH1
     `mailbox_data_subset` descriptor. **This required adding new
     provisioning surface that did not exist before** (see below).

4. **Amendment A6 (behavior change, not a signature change): at most one
   open snapshot per mailbox.** `OpenSnapshot()` must now return
   `ALIRO_INVALID_STATE` if a session is already open for the requested
   mailbox handle. Implemented in `mailbox_sessions.cpp::OpenSnapshot()`.
   This invalidated the old `test_multiple_sessions_stage_independently`
   test (it opened two concurrent sessions on the *same* mailbox handle,
   which is now rejected); replaced with
   `test_second_open_snapshot_on_same_mailbox_is_rejected` (same-mailbox
   rejection) and `test_sessions_on_different_mailboxes_stage_independently`
   (the still-valid cross-mailbox independence case).

## New provisioning surface added: AUTH1 `mailbox_data_subset`

Implementing `GetMailboxDataSubsetPairCount()`/`GetMailboxDataSubsetPair()`
honestly (rather than always reporting "not configured") required a place
to *provision* the descriptor. This was not previously scoped by AWP3/AWP6,
so a minimal, explicitly bounded extension was added:

- `AliroUd::Credential::MailboxDataSubsetPair` (`credential_types.h`): one
  `{ uint16_t mOffset; uint16_t mLength; }` pair.
- `AliroUd::Credential::MailboxConfig` gained `mDataSubsetConfigured`
  (bool), `mDataSubsetPairCount` (uint32_t), and `mDataSubsetPairs`
  (fixed-size array).
- New Kconfig `CONFIG_ALIRO_UD_MAILBOX_MAX_DATA_SUBSET_PAIRS` (default 4,
  range 0-32), in `storage/credential/Kconfig`. **This is deliberately not**
  the public `Aliro::UserDevice::kMaxMailboxDataSubsetPairs` (1863) - that
  constant is only the backend's provisioning-time *rejection* limit
  derived from the AUTH1 response-chaining budget (WP7-S2 decision D7 item
  7), not a size this small-flash application must support in full. If a
  future requirement needs more than the default 4 pairs, raise this
  Kconfig's default/range rather than the public constant.
- New CLI command `credential set-mailbox-data-subset <index> <offset>
  <length>`, mirroring `set-binding`'s indexed-slot staging pattern.
- New validation in `credential_store.cpp::ValidatePayloadShape()`: a
  subset pair's `[offset, offset+length)` must fit within the mailbox's own
  `mSizeBytes`, and `mDataSubsetConfigured` requires `mConfigured`.
- `mailbox inspect`'s CLI output gained `data_subset_configured=<0|1>
  data_subset_pairs=<n>` in place of the removed `settable_in_auth1=<0|1>`.
- The CLI-facing provisioning wire format's version
  (`AliroUd::Credential::Provisioning::kVersion`, `provisioning.h`) was
  bumped from `1` to `2`, since `Payload`'s (transitive) layout changed.
  **This means any previously-persisted staged/journaled payload blob
  encoded with v1 is now rejected as malformed** - acceptable for this
  fix, since Phase 1 has no cross-version persisted-data migration
  requirement, but worth knowing if a physical DK carries old test data.

**What was deliberately left as future work**, and should be picked up
by whichever AWP next touches mailbox provisioning (most likely a
revisit of AWP3's mailbox rights CLI, or a new AWP if the plan is amended):
- No DK/on-target demonstration of `mailbox_data_subset` provisioning was
  run as part of this fix (no physical NFC reader available, consistent
  with every prior AWP's noted hardware gap; and this is not a numbered
  AWP with its own DK-demonstration requirement).
- The exact encoding the stack expects when it actually emits the AUTH1
  `0x4B` tag (`GetMailboxDataSubsetPairCount()`'s `outConfigured`/
  `outCount` and `GetMailboxDataSubsetPair()`'s per-pair values) has only
  been exercised host-side, through `mailbox_engine.cpp`'s own WP7-S8 tests
  in the `ncs-aliro` repo - not through this application's own NFC
  transport end to end.

## Files changed by this fix pass

- `src/storage/credential/credential_types.h` - `MailboxConfig`/new
  `MailboxDataSubsetPair`/new `kMaxMailboxDataSubsetPairs`.
- `src/storage/credential/Kconfig` - new
  `CONFIG_ALIRO_UD_MAILBOX_MAX_DATA_SUBSET_PAIRS`.
- `src/storage/credential/provisioning.h` - `kVersion` bumped 1 -> 2.
- `src/storage/credential/credential_store.cpp` - subset-pair bounds
  validation in `ValidatePayloadShape()`.
- `src/storage/mailbox/mailbox_store.h`/`.cpp` - `Config` gained subset
  fields; `mSettableInAuth1` removed from `GetConfigLocked()`.
- `src/storage/mailbox/mailbox_sessions.h`/`.cpp` - `StageSet()` re-shaped;
  `OpenSnapshot()` now enforces amendment A6.
- `src/storage/mailbox/mailbox.cpp` - `StageSet()` forwarding updated; five
  new interface functions implemented.
- `src/cli/cli.cpp` - `set-mailbox`'s `rights` mask narrowed to 2 bits; new
  `set-mailbox-data-subset` command; `mailbox inspect` output field
  changed.
- `tests/functional/subsys/aliro_nfc_user_device/mailbox/src/{test_mailbox_store,test_mailbox_sessions}.cpp`
  and `tests/functional/subsys/aliro_nfc_user_device/cli_info/src/test_cli_info.cpp` -
  updated/added test coverage for all of the above.
- `docs/traceability.md` - one stale test-name citation corrected
  (`ALIRO-UD-SYRS-P1-033` row).
- `tests/functional/subsys/aliro_nfc_user_device/{command_timing,worker_lifecycle,authorization}/{CMakeLists.txt,Kconfig}` -
  these three now also build `storage/mailbox/{mailbox,mailbox_sessions,mailbox_store}.cpp`
  (+ `fake_mailbox_persistence.cpp`) and source `storage/mailbox/Kconfig`,
  mirroring `cli_info`'s existing pattern - required because the real
  `Aliro::UserDeviceStack`'s `HandleAuth1()`/`HandleExchange()` now
  unconditionally call `Aliro::Interface::UserDevice::Mailbox::*` (WP7-S7),
  which previously did not exist, so these tests never needed to link it.

**Not changed:** `ncs-aliro` itself (per `APP_PLAN.md`'s repository
boundary rule - this is the sibling application repository only).

## Verification performed for this fix pass

Run via `ncs4 west ...` from the west topdir
(`/home/mak5-local/gesture-access`), `ncs-aliro` at
`e5022e21b694a400d6349772185380a1bb5c5e8d` ("WP7-S9: complete-library
campaign, WP7 traceability closure"), unchanged by this fix (no `west
update` was run):

1. `west twister -T ncs-door-lock-and-access-control.git/tests/functional/subsys/aliro_nfc_user_device/mailbox -p native_sim/native/64`
   - **Pass**: 28 of 28 test cases (1 of 1 configuration).
2. `west twister -T ncs-door-lock-and-access-control.git/tests/functional/subsys/aliro_nfc_user_device -p native_sim/native/64`
   (the whole suite) - **6 of 9 configurations pass** (`mailbox`,
   `command_timing`, `command_timing_disabled`, `host_smoke`, `apdu_fragment_assembler`,
   and one more; 74 of 110 test cases). The remaining 3 -
   `worker_lifecycle`, `authorization`, `cli_info` - **fail to link**, but
   on `Aliro::Interface::UserDevice::Crypto::*`/`CredentialSigning::*`
   symbols (`DestroyKey`, `Sign`, `GetPublicKey`,
   `GenerateEphemeralKeyPair`, `Sha1`/`Sha256`, `ImportKey`,
   `RawKeyAgreement`, `AeadEncrypt`/`AeadDecrypt`, `ValidateCertificate`,
   `DeriveRawKey`, `ValidateCertificate`) - **not** `Mailbox::*` symbols.
   This is exactly the pre-existing, unrelated bug AWP7's evidence already
   documented ("External stack observations": these three tests'
   `CMakeLists.txt`/`Kconfig` never linked `platform/crypto/*.cpp`, and the
   stack's session-teardown path unconditionally references
   `Crypto`/`CredentialSigning` even for SELECT-only traffic; adding the
   crypto backend uncovers a second, also-pre-existing
   `mutexes cannot be used inside ISRs` assertion in the stack's own
   watchdog-teardown path). **Confirmed still true, still unrelated to
   this fix, and still out of scope for this application to fix** per
   `APP_PLAN.md`'s stack-boundary rule - left exactly as found, as AWP7
   also chose to do.
3. `west build -p -b nrf54lm20dk/nrf54lm20a/cpuapp -d /tmp/build-aliro-wp7-fix ncs-door-lock-and-access-control.git/applications/aliro-nfc-user-device`
   - **Pass.** FLASH 186468 B (8.94%), RAM 121280 B (23.18%) - the real
     application (unlike the three minimal test harnesses above) already
     links the full `platform/crypto/*.cpp` backend, so it is unaffected by
     item 2's issue.

## What still needs to happen before resuming AWP work

1. Update `docs/evidence.md` with a dated entry recording this fix pass and
   the verification above (this document explains *what* and *why*;
   `docs/evidence.md` is still the place for command-level pass/fail
   evidence per `APP_PLAN.md` §4).
2. Re-record the tested `ncs-aliro` revision
   (`e5022e21b694a400d6349772185380a1bb5c5e8d`) in `docs/evidence.md`, per
   `APP_PLAN.md`'s "After any stack update" rule.
3. No DK-hardware demonstration of the new `mailbox_data_subset` CLI
   command or the amendment-A6 single-session behavior was run (no
   physical NFC reader available in this environment, consistent with
   every prior AWP; and `mailbox inspect`/`credential set-mailbox-data-subset`
   are plain CLI commands with no NFC dependency, so this is a minor gap,
   not a blocker).
4. Only then resume the next `TARGET_AWP` per `APP_PLAN.md`.
