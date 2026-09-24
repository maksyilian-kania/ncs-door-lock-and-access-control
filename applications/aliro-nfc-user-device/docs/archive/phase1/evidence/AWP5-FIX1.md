# AWP5-FIX1 — Full-size User Device AEAD payloads

**Not a numbered AWP.** Targeted fix against already-committed `AWP5`
(`docs/evidence/AWP5.md`, read-only/historical — linked, not restated).

## Defect

`src/platform/crypto/crypto.cpp`'s `AeadEncrypt()` capped plaintext at a
hardcoded `kMaxAeadPlainTextLength{ 512 }`, rejecting any longer input with
`ALIRO_INVALID_ARGUMENT`. The checked-out `ncs-aliro` stack's pre-chaining
command/response capacity (`kPreChainingMaxSize` in
`stack/src/user_device/access_protocol/wire_types.h`) is 2048 bytes, sized
to the Aliro spec's >=2000-byte oversize-APDU minimum (§8.3.2.2, p. 62). Any
AUTH1/EXCHANGE payload the stack assembles between 513 and 2048 bytes was
silently rejected by the application's crypto layer before chaining could
even apply.

## Fix

- `kMaxAeadPlainTextLength` raised from `512` to `2048`
  (`applications/aliro-nfc-user-device/src/platform/crypto/crypto.cpp`). The
  PSA one-shot scratch buffer (`PSA_AEAD_ENCRYPT_OUTPUT_SIZE(...,
  kMaxAeadPlainTextLength)`) resizes automatically from the same constant.
  No change to keys, nonces, counters, tag size, or logging.
- `tests/functional/subsys/aliro_nfc_user_device/crypto/src/test_crypto.cpp`:
  added `test_aead_round_trips_pre_chaining_payload_lengths` (512/513/2048-byte
  round trip through the real PSA provider) and
  `test_aead_encrypt_failures_leave_caller_buffers_unchanged` (oversized-input
  rejection and a provider-failure path both leave caller output buffers
  untouched).

## Prerequisite/scope deviations (flagged, not silently resolved)

- The fix-request document names sibling finding `WP7-R23` as the defect
  record and asks for the stack pinned at
  `1966c328934f12a8e4eec25209f40d8ba5c9696e`. Neither holds in this
  environment: `ncs-aliro/docs/execution/findings.yaml`'s actual `WP7-R23`
  is an unrelated, already-`fixed` run-record checksum finding, and revision
  `1966c328...` does not exist in the checked-out `ncs-aliro` (its remote is
  unreachable from this sandbox to check further). Per `APP_PLAN.md`'s own
  rule ("use the currently explicitly checked out revision of `ncs-aliro`
  ... MUST NOT silently run `west update`"), this pass used the
  already-checked-out revision `198def4e6a66144049a7fd1f56378b9cbba7a7c0`
  (unchanged, confirmed via `git -C ncs-aliro rev-parse HEAD`) and did not
  attempt to switch. The AEAD-cap defect itself was independently confirmed
  by direct inspection of `crypto.cpp` and `wire_types.h`, not by trusting
  the `WP7-R23` cross-reference.
- No physical DK is attached in this environment (`/dev/ttyACM*` absent), so
  the physical `NFC_UD_EXCHANGE_WITH_CHAINING` harness run required by the
  fix request's exit checks was **not performed**. All other checks below
  were run.

## Commands run and results

Toolchain invoked via `ncs4`, from the repository root
(`/home/mak5-local/gesture-access/ncs-door-lock-and-access-control.git`).

1. `west twister -p native_sim/native/64 -vv -T tests/functional/subsys/aliro_nfc_user_device/crypto`
   — **Result: pass.** 1/1 test suite, 24/24 test cases, including both new
   boundary cases.
2. `git diff --check ab5fffb1fb75361610c9d1be41c7df8e34e24291..HEAD` —
   **exit 0.**
3. `west build -p always -b nrf54lm20dk/nrf54lm20b/cpuapp applications/aliro-nfc-user-device`
   — **Result: pass.** FLASH 186772 B (8.96%), RAM 121280 B (23.18%).
4. `west build -p always -b nrf54lm20dk/nrf54lm20b/cpuapp -d build_crypto_dk tests/functional/subsys/aliro_nfc_user_device/crypto`
   — **Result: pass (compiled for the DK target).** FLASH 132516 B (6.36%),
   RAM 63848 B (12.20%). Not flashed or run — no DK attached (see above).

## Outstanding items

- Physical DK flash + `aliro_ud_crypto` on-target run, and the
  `NFC_UD_EXCHANGE_WITH_CHAINING` harness case, remain unperformed pending
  hardware access. Per `APP_PLAN.md`'s invocation contract, the sibling
  finding `WP7-R23`-style closure (fix-request exit check 5) should not be
  claimed until that physical evidence exists.
- The `WP7-R23`/stack-revision naming mismatch above should be reconciled
  with whoever authored the fix-request document.

## Follow-up hardware attempt

- `ncs4 west flash -d build_crypto_dk` — **Result: pass.** Nordic board
  `1051885995` was detected and the DK crypto-test image was programmed and
  verified successfully.
- Target UART capture was attempted on `/dev/ttyACM0` and `/dev/ttyACM1`
  after the flash. Those device nodes were not accessible in the shell
  environment (`ENOENT`), so the on-target test result could not be
  collected.
- The physical `NFC_UD_EXCHANGE_WITH_CHAINING` harness remains unperformed:
  only the User Device DK was visible to `nrfutil device list`, and the
  Reader/harness UART was unavailable.
