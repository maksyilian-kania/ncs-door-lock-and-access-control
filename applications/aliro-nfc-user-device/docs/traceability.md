# Aliro NFC User Device — Requirement Traceability

This table maps every Phase 1 requirement (`docs/Requirements.pdf`,
`ALIRO-UD-SYRS-P1-001` through `-040`) to the application files that
implement it, stack ownership, tests, DK evidence, the normative citation
used to resolve ambiguity, and one status:

- `app-implemented` — the application-owned portion is implemented and
  covered by host tests.
- `verified-end-to-end` — an on-target/end-to-end check against the
  checked-out stack passed.
- `not-yet-verifiable` — the application and/or the checked-out stack does
  not yet implement the behavior needed to verify this requirement.
- `blocked-external-contract` — the checked-out public `ncs-aliro` contract
  cannot represent the required behavior; the exact missing contract is
  named in `docs/evidence.md`.

Created in AWP0 per `APP_PLAN.md` §1; updated in every subsequent AWP. AWP0
implemented no protocol requirement — it replaced the POC skeleton, enabled
the checked-out User Device stack, and created the module layout later AWPs
fill in.

AWP1 implements `platform/nfc` (bounded queue + dedicated NFC/stack worker
thread, ISO-DEP fragment reassembly, `Interface::UserDevice::Nfc`) and
`platform/os` (mutex, timer, deferred-event queue, trusted-time,
role-neutral `Interface::Logging` bridge). Per `APP_PLAN.md` AWP1 scope,
this covers the *application transport* portions of `-001`/`-002`; `-013`
and `-014` remain stack-owned but are now exercisable end-to-end through
this transport (see the AWP0 status legend above). Every other row is
unchanged from AWP0 (`not-yet-verifiable`).

**Post-AWP1 lifecycle-race hardening pass**: `platform/nfc`
was reworked to make FIELD_ON/FIELD_OFF idempotent, to synchronize the
application's local session-active belief with independent stack-driven
termination, to reject command APDUs deterministically when no session is
believed active, to never silently drop a lifecycle transition (coalesced
field intent channel + forced session recovery on queue overflow, replacing
the previous "drop everything on overflow" model), and to remove the
`sAssembler` cross-thread race between the `nfc_t4t_lib` callback and the
worker thread. See `docs/evidence.md` for the detailed change list and test
results; the `-002`/`-013` rows below are unchanged in status but their
supporting `worker_lifecycle` test evidence now also covers this hardened
lifecycle behavior.

**AWP2** implements `src/cli` (Zephyr shell on the DK virtual UART,
`aliro-ud` root command, `info` command, credential staging command
shells) per `APP_PLAN.md` AWP2 scope, covering `-003`. It also adds
placeholder (not-yet-implemented) `src/storage/credential` and
`src/platform/authorization` implementations — needed only because the
checked-out `ncs-aliro` moved to WP5, whose `CredentialManager`/
`EventHandler` now call those contracts unconditionally — so the
application and every test linking the real stack continue to build; see
`docs/evidence.md` for why these are stubs, not real behavior.

**AWP3** implements `src/storage/credential` (Zephyr settings/NVS-backed
metadata, PSA/CRACEN/KMU-backed private keys, a four-phase crash-safe
transaction journal, per-binding `{reader_group_identifier, trust_type,
reader_group_identifier_key}` trust storage, preferred-credential
tracking) and `src/lifecycle` (the mutating-operation coordinator), after
`ncs-aliro` WP5.5 made `Trust::GetReaderPublicKey()`/
`GetReaderIssuerPublicKey()` binding-aware and unblocked the gap
originally flagged against WP5 (see `docs/evidence.md`'s "Resolution (out
of band, before AWP4)" note), covering `-004` through `-010`.

**AWP4** implements `src/platform/authorization` (a device-global,
host-testable button-authorization window; the real `Authorization`
contract adapter; DK button/LED GPIO backends) and extends `src/cli` with
`aliro-ud auth status|press|clear|notify-required`, covering the
application portions of `-011`, `-012`, `-020`, and `-021`. Getting the
physical DK demonstration running also surfaced and fixed a real on-target
crash (main-thread stack overflow) and an unsafe ISR-context logging path
in the button backend; see `docs/evidence.md`.

**AWP5** implements `src/platform/crypto` (`Aliro::Interface::UserDevice::Crypto`
thin PSA Crypto bindings, profile0000 Reader certificate
decompression/validation, `CredentialSigning::Sign()`), covering the
application/platform portions of `-008` (session/ephemeral-key), `-018`,
`-022` through `-027`, and `-038`. All 17 host test cases also passed
unchanged on a physical nRF54LM20 DK against its real `nrf_security` PSA
driver; see `docs/evidence.md`.

**AWP6** implements `src/storage/mailbox` (the
`Aliro::Interface::UserDevice::Mailbox` snapshot/staged-mutation/atomic-
commit/rollback/close session engine, plus a Credential-Issuer-level
`Store` layer backed by Zephyr settings/NVS) and extends `src/cli` with
`aliro-ud mailbox inspect|read|init|reset`, covering the application/
backend portions of `-032` through `-034`. A physical DK became available
partway through this AWP: the 26-case mailbox host suite was re-run
unchanged on-target, and a full interactive session over the shell UART
provisioned a real credential+mailbox, inspected/initialized/read/reset
it, and confirmed both the credential and its mailbox survive a real
board reset and that `credential delete` erases the mailbox — surfacing
and fixing a pre-existing `CONFIG_SHELL_STACK_SIZE` stack-overflow bug on
`credential commit` in the process (first-ever interactive DK `commit`;
see `docs/evidence.md`'s AWP6 "On-target bug found and fixed"). No NFC
reader was available, so the Reader-driven EXCHANGE path itself remains
undemonstrated on-target.

| ID | Requirement (summary) | App files (this AWP) | Stack ownership | Tests | DK evidence | Normative citation | Status |
|---|---|---|---|---|---|---|---|
| ALIRO-UD-SYRS-P1-001 | Execute on nRF54LM20B DK + PCA64110 antenna. | `CMakeLists.txt`, `prj.conf`, `src/main.cpp`, `src/platform/nfc`, `src/platform/os` | — | `build.aliro_nfc_user_device` (twister, DK target build) | Flashed to a physical nRF54LM20 DK (serial 1051885995) and booted; console log shows stack init and NFC T4T listen mode start (see `docs/evidence.md`) | NORDIC-DK; project constraint | verified-end-to-end |
| ALIRO-UD-SYRS-P1-002 | Operate as NFC-A Type 4 Tag Platform / ISO-DEP listen mode. | `src/platform/nfc/nfc_transport.{h,cpp}`, `nfc_worker.{h,cpp}`, `apdu_fragment_assembler.{h,cpp}` | SELECT/ISO-DEP session behavior | `apdu_fragment_assembler` (fragment assembly/overflow/reset, host), `worker_lifecycle` (12 cases: idempotent field on/off, stack-driven timeout termination, duplicate/stale events, field-loss race, queue overflow forcing session recovery without dropping FIELD_OFF, APDU rejection with no active session, host, real stack) | NFC T4T listen mode confirmed started on DK console log (see `docs/evidence.md`); no physical reader/antenna tap performed yet | ALIRO-SPEC §10.1, p.93; NORDIC-NFC | app-implemented |
| ALIRO-UD-SYRS-P1-003 | Line-oriented CLI over DK virtual UART, 115200 8N1. | `src/cli/cli.cpp`, `src/platform/os/app_status.{h,cpp}`, `prj.conf` (`CONFIG_SHELL=y`) | — | `cli_info` (3 cases: `info` fields/format, live session-state reflection, credential-shell `ERR NOT_IMPLEMENTED` responses; host, native shell/dummy backend, real stack) | Flashed to physical nRF54LM20 DK (serial 1051885995) and driven over the shell virtual COM port at 115200-8N1: `aliro-ud info`, `credential begin-create/set-key/commit/abort` (all deterministic `OK`/`ERR NOT_IMPLEMENTED` lines), and command-tree/`help` listing (see `docs/evidence.md`) | Project constraint; NORDIC-DK | verified-end-to-end |
| ALIRO-UD-SYRS-P1-004 | CLI provisions Access Credential (key, bindings, trust, policy, timestamps, mailbox config, optional documents). | `src/storage/credential`, `src/cli/cli.cpp` (staging transaction + `commit`) | — | `cli_info::test_credential_staging_transaction_commits` (host, real stack) | Flashed to physical nRF54LM20 DK (serial 1051885995) and driven over the shell UART (AWP6 pass): `begin-create`/`set-key`/`set-policy`/`set-binding`/`set-mailbox`/`commit` -> `OK handle=1`, real PSA-backed key import and Zephyr settings/NVS commit on real flash (see `docs/evidence.md` AWP6, item 4) | ALIRO-SPEC §§6.2, 8.3.1.14–15 | verified-end-to-end |
| ALIRO-UD-SYRS-P1-005 | CLI create/update/inspect/delete/factory-reset with deterministic results. | `src/storage/credential/credential_store.{h,cpp}`, `src/cli/cli.cpp` | — | `cli_info::test_credential_staging_transaction_commits` (create/inspect/list/delete, host, real stack) | Same DK session as `-004`: `credential list`/`inspect 1` reflected the committed record, `credential delete 1` -> `OK` then `list` -> `OK count=0` (see `docs/evidence.md` AWP6, item 4) | Project constraint | verified-end-to-end |
| ALIRO-UD-SYRS-P1-006 | Reject malformed provisioning input without changing committed state. | `src/storage/credential/credential_store.cpp` (`ValidatePayloadShape()`, validate-before-commit) | `CredentialManager::Create/Update()` validate-before-commit (WP5.5, D7) | `cli_info` host suite (host, real stack) | — | Project constraint; security objective | app-implemented |
| ALIRO-UD-SYRS-P1-007 | Retain credentials/trust/policies/documents/mailbox/Kpersistent across reset and power loss. | `src/storage/credential/{credential_store,credential_persistence_settings,key_backend_psa}.cpp` (Zephyr settings/NVS + PSA/CRACEN/KMU, 4-phase journal) | — | Journal/persistence logic implemented; no host fault-injection test suite dedicated to this behavior yet | Flashed to physical nRF54LM20 DK (AWP6 pass): after a real `RESET_SYSTEM` reset, `credential list`/`inspect 1` still reported the committed credential (`has_mailbox=1` etc.) and its mailbox's initialized/zeroed state, confirming real cross-reset persistence on flash (mailbox portion only; general credential-field power-loss fault injection remains a host-test gap, see `docs/evidence.md` AWP6) | ALIRO-SPEC §§3.1, 6.2, 8.3.1.15 | app-implemented |
| ALIRO-UD-SYRS-P1-008 | Never export Access Credential private key, Kpersistent, or session key via CLI/NFC/logs. | `src/platform/crypto/{crypto,certificate,credential_signing}.cpp` (AWP5: every PSA key is referenced only by opaque `KeyId`; `CredentialSigning::Sign()` resolves a `CredentialHandle` to its PSA key and signs through `KeyBackend::Sign()` without ever copying the scalar into this module), `src/storage/credential` (opaque PSA key IDs only, never a raw scalar), `src/storage/mailbox` (AWP6: mailbox bytes are application-owned non-secret payload, never a key) | Kpersistent generation/storage (WP6 orchestration; still unimplemented) | AWP1-6 "Security check" grep review of every changed file each AWP (see `docs/evidence.md`); `aliro_ud_credential_signing` suite; `mailbox` suite | — | ALIRO-SPEC §16.2; NORDIC-SEC | app-implemented (credential-storage, session/ephemeral-key, and mailbox portions; Kpersistent generation/derivation remains unimplemented, stack-owned) |
| ALIRO-UD-SYRS-P1-009 | Support at least 16 `reader_group_identifier` bindings per Access Credential. | `src/storage/credential/credential_types.h` (`kMaxBindingsPerCredential`, default 16), `credential_store.cpp` | — | No dedicated 16-binding-capacity test recorded yet (evidence gap, see `docs/evidence.md`'s "Resolution" note) | — | ALIRO-SPEC §6.2 | app-implemented |
| ALIRO-UD-SYRS-P1-010 | CLI method to select among multiple matching Access Credentials. | `src/storage/credential/credential_store.cpp` (`SetPreferredCredential`/`GetPreferredCredential`/`ResolveByReaderGroupIdentifier`), `src/cli/cli.cpp` (`credential preferred-set/preferred-get`) | `Trust::GetReaderPublicKey/GetReaderIssuerPublicKey` now binding-aware (`ncs-aliro` WP5.5, decision D9 — resolves the AWP3 blocker note) | No dedicated preferred-selection test recorded yet (evidence gap, see `docs/evidence.md`'s "Resolution" note) | — | ALIRO-SPEC §6.2; ALIRO-TP Table 4-3 | app-implemented |
| ALIRO-UD-SYRS-P1-011 | DK button press = user authentication for a configurable 1–300 s window (default 30 s). | `src/platform/authorization/{authorization_window,authorization,authorization_button}.{h,cpp}`, `Kconfig` (`CONFIG_ALIRO_UD_AUTHORIZATION_WINDOW_SECONDS`, range 1-300, default 30) | — | `authorization` suite (17 cases: window boundaries/retry/preauthorization with a fake monotonic clock, contract-level `GetState()`, a real-AUTH0-through-real-stack e2e case; host) | Performed: real presses of DK `Button 0` opened, re-extended, and expired the window, cross-checked against `aliro-ud auth status` over the shell UART (see `docs/evidence.md` AWP4 entry, which also documents an on-target main-thread stack-overflow fix required to get the shell UART working) | Project architecture; ALIRO-SPEC §8.3.1.14 | verified-end-to-end |
| ALIRO-UD-SYRS-P1-012 | Visible indication when authentication is required and no valid window exists. | `src/platform/authorization/{authorization_indicator.h,authorization_led,authorization}.cpp` | — | `authorization` suite: `NotifyAuthenticationRequired()`/indicator-activation cases (host) | Performed via the new `aliro-ud auth notify-required` CLI test trigger (no NFC reader available to drive AUTH0 end to end): LED0 lit then cleared by a real button press, confirmed by a debug-probe GPIO register read and the user's visual observation (see `docs/evidence.md` AWP4 entry) | ALIRO-SPEC §8.3.1.14 | verified-end-to-end |
| ALIRO-UD-SYRS-P1-013 | SELECT of AID `A000000909ACCE5501` returns conforming FCI (type `0x0000`, version `0x0100`, ≤256 bytes). | `src/platform/nfc` (transport wiring only; FCI bytes are stack-owned) | FCI/SELECT response encoding | `worker_lifecycle::test_select_expedited_aid_returns_spec_fci` (exact FCI bytes from the Aliro spec worked example, through the real worker + real stack, host) | Not yet exercised with a physical reader/antenna tap | ALIRO-SPEC §10.2.1.2 | verified-end-to-end (host); DK antenna tap pending |
| ALIRO-UD-SYRS-P1-014 | Encode expedited commands/responses as ISO/IEC 7816-4 APDUs per Aliro CLA/INS/P1/P2/Lc/Le/status words/TLV/endianness. | — | APDU/TLV encoding | — | — | ALIRO-SPEC §8.3.2.1 | not-yet-verifiable |
| ALIRO-UD-SYRS-P1-015 | Support command chaining for AUTH0 responses and LOAD CERT/AUTH1/EXCHANGE. | `src/platform/nfc/apdu_fragment_assembler.{h,cpp}` (raw ISO-DEP fragment reassembly only; Aliro-level command chaining is stack-owned) | Chaining state machine | `apdu_fragment_assembler::test_chained_fragments_assemble_in_order` and related cases (host) | — | ALIRO-SPEC §8.3.2.2; ALIRO-TP Table 4-3 | app-implemented (transport-level reassembly only) |
| ALIRO-UD-SYRS-P1-016 | Accept unknown extension TLVs where forward-compatible processing is required. | — | TLV parsing | — | — | ALIRO-SPEC §§8.3.3.2, 8.3.3.4, 10.2.1.2 | not-yet-verifiable |
| ALIRO-UD-SYRS-P1-017 | Support complete Expedited Standard flow: SELECT, AUTH0, optional LOAD CERT, AUTH1, EXCHANGE\*, result handling. | — | Session/Access Protocol state machine | — | — | ALIRO-SPEC §§8.1.1.1, 8.2 | not-yet-verifiable |
| ALIRO-UD-SYRS-P1-018 | Generate new random P-256 ephemeral Access Credential key pair per AUTH0 response. | `src/platform/crypto/crypto.cpp` (`GenerateEphemeralKeyPair()`: `PSA_KEY_TYPE_ECC_KEY_PAIR(SECP_R1)`, volatile lifetime, AWP5) | AUTH0 response orchestration (calling this once per response) not yet implemented in the checked-out stack (WP6) | `aliro_ud_crypto::test_ephemeral_ecdh_agreement_matches_on_both_sides` (host + DK) | Ran on a physical nRF54LM20 DK (serial 1051885995) over the shell UART: full 17-case AWP5 host suite passed unchanged on-target, confirming the real hardware-accelerated PSA driver path (see `docs/evidence.md` AWP5 entry) | ALIRO-SPEC §§8.3.3.2.6–7 | app-implemented |
| ALIRO-UD-SYRS-P1-019 | Select Access Credential/trust material using AUTH0 `reader_group_identifier`. | `src/storage/credential/credential_store.cpp` (`ResolveByReaderGroupIdentifier`, `GetReaderPublicKey`/`GetReaderIssuerPublicKey`) | AUTH0 parsing, `CredentialManager::EvaluateAuth0()`/`ResolveReaderTrust()` orchestration | `authorization_e2e` (real AUTH0 -> real `CredentialManager` -> real `credential_store` lookup, host) | — | ALIRO-SPEC §§6.2, 8.3.3.2.6 | app-implemented |
| ALIRO-UD-SYRS-P1-020 | Enforce `authentication_policy` values `0x01`/`0x02`/`0x03` per selected Access Credential policy. | `src/platform/authorization/{authorization_window,authorization}.{h,cpp}` (uniform window gate; `GetState()` has no policy parameter, so combining the Reader-requested and credential-provisioned policy values is entirely stack-owned) | `CredentialManager::EvaluateAuth0()` policy combination (stack's own `test_policy.cpp` covers 0x01/0x02/0x03 directly) | `authorization_e2e::test_auth0_policy_0x03_*` (application portion, host, real stack) | Not yet performed: the DK demonstration available this AWP (see `docs/evidence.md`) exercised the button/LED window mechanics via CLI test triggers, not a real AUTH0 exchange (no NFC reader in this environment) — policy-value combination itself is unverified on-target | ALIRO-SPEC §8.3.1.14 | app-implemented |
| ALIRO-UD-SYRS-P1-021 | Policy `0x03` continues only with a valid button authorization window. | `src/platform/authorization/{authorization_window,authorization}.{h,cpp}` | Policy gate invocation (`CredentialManager::EvaluateAuth0()`) | `authorization` suite: `test_auth0_policy_0x03_with_no_window_indicates_required`/`test_auth0_policy_0x03_with_open_window_does_not_indicate` (host, real stack) | Not yet performed: same limitation as `-020` above — no NFC reader in this environment to drive a real AUTH0 exchange on-target | ALIRO-SPEC §8.3.1.14; project architecture | app-implemented |
| ALIRO-UD-SYRS-P1-022 | No-certificate case: authenticate Reader via AUTH1 signature with bound `reader_PubK`. | `src/storage/credential` (trust lookup, AWP3), `src/platform/crypto/crypto.cpp` (`VerifySignature()`: ECDSA P-256/SHA-256, AWP5) | AUTH1 field assembly and signature-verification call sequencing (WP6) | `aliro_ud_crypto::test_verify_signature_accepts_valid_and_rejects_tampered` (host + DK) | See `-018` DK run | ALIRO-SPEC §8.3.3.4.5 | app-implemented |
| ALIRO-UD-SYRS-P1-023 | Certificate case: authenticate Reader via certificate subject key only after issuer-CA verification. | `src/platform/crypto/{certificate,crypto}.cpp` (`ValidateCertificate()`/profile0000 decompression + `VerifySignature()` against the extracted subject key, AWP5), `src/storage/credential` (issuer-CA trust lookup, AWP3) | LOAD CERT/AUTH1 orchestration (WP6) | `aliro_ud_certificate` suite (5 cases, host + DK) | See `-018` DK run | ALIRO-SPEC §§6.3.1, 8.3.3.3, 8.3.3.4.5 | app-implemented |
| ALIRO-UD-SYRS-P1-024 | Reject invalid/untrusted Reader certificate without disclosing trust-material existence. | `src/platform/crypto/certificate.cpp` (`Validate()` returns one of a small fixed set of error codes — `ALIRO_INVALID_DATA_FORMAT`/`ALIRO_INVALID_SIGNATURE`/`ALIRO_ERROR_INTERNAL` — from every failure path; none is conditioned on whether `issuerPublicKey` corresponds to a provisioned binding) | Certificate/session failure handling (mapping to the wire failure process is WP6) | `aliro_ud_certificate::test_validate_rejects_wrong_issuer_key`/`test_validate_rejects_tampered_signature`/`test_validate_rejects_truncated_certificate` (host + DK) | See `-018` DK run | ALIRO-SPEC §§8.3.3.3.4, 8.3.3.4.5–6 | app-implemented |
| ALIRO-UD-SYRS-P1-025 | Derive Kdh/ExpeditedSKReader/ExpeditedSKDevice/StepUpSK via ECKA-DH + HKDF-HMAC-SHA-256 (byte `0x5E`). | `src/platform/crypto/crypto.cpp` (`RawKeyAgreement()`: `PSA_ALG_ECDH`; `DeriveSymmetricKey()`/`DeriveRawKey()`: `PSA_ALG_HKDF(PSA_ALG_SHA_256)` — thin PSA bindings only; the `0x5E` info-byte/label construction and multi-key derivation sequencing are stack-owned, AWP5) | KDF construction/sequencing (which `info`/`salt` bytes to pass, and the ECKA-DH-vs-HKDF staging in between) | `aliro_ud_crypto::test_ephemeral_ecdh_agreement_matches_on_both_sides`, `test_derive_symmetric_key_round_trips_through_aead`, `test_derive_raw_key_is_deterministic_for_same_inputs` (host + DK) | See `-018` DK run | ALIRO-SPEC §§8.3.1.4–5, 8.3.1.13 | app-implemented |
| ALIRO-UD-SYRS-P1-026 | Protect AUTH1/EXCHANGE responses with AES-GCM, direction-specific keys, session counter. | `src/platform/crypto/crypto.cpp` (`AeadEncrypt()`/`AeadDecrypt()`: `PSA_ALG_GCM`, thin PSA binding only, AWP5) | AEAD orchestration/counters/direction-key selection (WP6) | `aliro_ud_crypto::test_derive_symmetric_key_round_trips_through_aead` (encrypt/decrypt round trip plus a tampered-ciphertext rejection case; host + DK) | See `-018` DK run | ALIRO-SPEC §§8.3.1.6–11, 8.3.3.4.6, 8.3.3.5.5 | app-implemented |
| ALIRO-UD-SYRS-P1-027 | Generate 64-byte ECDSA P-256/SHA-256 User Device signature over exact AUTH1 fields using the Access Credential private key. | `src/platform/crypto/credential_signing.cpp` (`CredentialSigning::Sign()`: resolves `CredentialHandle` -> `KeyId` via `credential_store::GetFullRecord()`, signs via `KeyBackend::Sign()`, AWP5), `src/storage/credential` (opaque-handle signing, AWP3) | AUTH1 field assembly (which exact bytes get signed) is stack-owned (WP6) | `aliro_ud_credential_signing` suite (3 cases: signs against a real provisioned credential and the signature is independently checked with `Crypto::VerifySignature()`; host + DK) | See `-018` DK run | ALIRO-SPEC §§8.3.1.2, 8.3.3.4.3, 8.3.3.4.6 | app-implemented |
| ALIRO-UD-SYRS-P1-028 | Return Access Credential public key or `key_slot` per AUTH1 parameter. | `src/storage/credential` (skeleton only) | AUTH1 response assembly | — | — | ALIRO-SPEC §8.3.3.4.2 | not-yet-verifiable |
| ALIRO-UD-SYRS-P1-029 | Return accurate `signaling_bitmap` (documents, mailbox state/rights, optional EXCHANGE capabilities). | `src/storage/credential`, `src/storage/mailbox` (skeletons only) | AUTH1 response assembly | — | — | ALIRO-SPEC §8.3.3.4.2 | not-yet-verifiable |
| ALIRO-UD-SYRS-P1-030 | Support `credential_signed_timestamp`/`revocation_signed_timestamp` in AUTH1 response when provisioned. | `src/storage/credential` (skeleton only) | AUTH1 response assembly | — | — | ALIRO-SPEC §8.3.3.4.2; ALIRO-TP Table 4-3 | not-yet-verifiable |
| ALIRO-UD-SYRS-P1-031 | AUTH0/AUTH1 externally observable data independent of credential/reader-key/Kpersistent existence, except protocol-defined result. | `src/storage/credential`, `src/platform/crypto` (skeletons only) | AUTH0/AUTH1 response shaping | — | — | ALIRO-SPEC §§8.3.3.2.6, 8.3.3.4.6 | not-yet-verifiable |
| ALIRO-UD-SYRS-P1-032 | Persistent mailbox per Access Credential with provisionable size, read/write permission, optional AUTH1 subset. | `src/storage/mailbox/{mailbox_types,mailbox_persistence,mailbox_persistence_settings,mailbox_store}.{h,cpp}` (Zephyr settings/NVS-backed committed byte storage, one slot per credential, live size/permissions read from `credential_store`) | — | `mailbox` suite (26 cases, host, real stack); `cli_info::test_mailbox_inspect_init_read_and_erase_on_delete` | Flashed to physical nRF54LM20 DK (serial 1051885995) and driven over the shell UART at 115200-8N1: provisioned a real credential with `set-mailbox 8 3`, `mailbox init`/`read`/`reset`, and confirmed both the credential and its mailbox's initialized/zeroed state survive a real `RESET_SYSTEM` reset (real Zephyr settings/NVS flash, not a fake); `credential delete` confirmed to erase the mailbox (see `docs/evidence.md` AWP6) | ALIRO-SPEC §8.3.1.15 | verified-end-to-end |
| ALIRO-UD-SYRS-P1-033 | Execute authorized mailbox read/write/set at reader-selected offsets through EXCHANGE. | `src/storage/mailbox/{mailbox_sessions,mailbox}.{h,cpp}` (`Aliro::Interface::UserDevice::Mailbox` adapter: `Read`/`StageWrite`/`StageSet`, bounds/overflow-checked, `MailboxPermissions`-gated) | EXCHANGE parsing/orchestration (which offsets/requests the Reader sent; not yet implemented in the checked-out stack, WP6) | `mailbox` suite: `test_stage_write_rejects_out_of_bounds_and_overflowing_ranges`, `test_stage_set_requires_exact_full_mailbox_length`, `test_read_rejected_when_not_readable`, `test_stage_write_rejected_when_not_writable` (host, real stack) | Same DK run as `-032` exercised only the Credential-Issuer-level `Store` path (CLI), not a Reader-driven session; no NFC reader available in this environment to drive a real EXCHANGE (see `docs/evidence.md` AWP6 "Outstanding items") | ALIRO-SPEC §§8.3.1.15, 8.3.3.5 | app-implemented (session engine only; not yet exercised via a real EXCHANGE) |
| ALIRO-UD-SYRS-P1-034 | Commit all write/set requests in one EXCHANGE atomically; preserve pre-session data until close. | `src/storage/mailbox/mailbox_sessions.cpp` (`Commit()`: applies a session's entire staged buffer to committed storage in one `Store::ApplyDirtyBytes()` call; `Rollback()`/`Close()` leave committed bytes unchanged) | EXCHANGE session boundary (when to call `Commit()` vs `Rollback()` based on the Reader's final status; WP6) | `mailbox` suite: `test_read_isolated_from_staged_writes_until_commit`, `test_rollback_and_close_leave_committed_bytes_unchanged`, `test_multiple_sessions_stage_independently`, plus 4 fault-injection cases covering commit-failure/reboot-recovery (host, real stack) | All 26 `mailbox` host cases re-run unchanged on the physical DK (see `docs/evidence.md` AWP6, item 3); EXCHANGE-level atomicity itself not yet demonstrated (no reader) | ALIRO-SPEC §§8.3.1.15, 8.3.3.5.4 | app-implemented |
| ALIRO-UD-SYRS-P1-035 | Return Aliro secure-channel error sequence on unauthorized/out-of-bounds/malformed/failed EXCHANGE. | `src/storage/mailbox` (skeleton only) | Secure-channel error sequencing | — | — | ALIRO-SPEC §8.3.3.5.5 | not-yet-verifiable |
| ALIRO-UD-SYRS-P1-036 | Return secure-channel success sequence only after all EXCHANGE requests succeed. | `src/storage/mailbox` (skeleton only) | Secure-channel success sequencing | — | — | ALIRO-SPEC §8.3.3.5.5 | not-yet-verifiable |
| ALIRO-UD-SYRS-P1-037 | Process final Reader status in EXCHANGE / unencrypted CONTROL FLOW failure and terminate transaction. | `src/platform/nfc` (skeleton only) | Transaction termination sequencing | — | — | ALIRO-SPEC §§8.3.3.5, 10.2–10.2.2 | not-yet-verifiable |
| ALIRO-UD-SYRS-P1-038 | On Aliro failure: empty response data, applicable error status, destroy session-bound keys/data, terminate transaction. | `src/platform/nfc` (skeleton only), `src/platform/crypto/crypto.cpp` (`DestroyKey()`: idempotent `psa_destroy_key()`, treats `PSA_ERROR_INVALID_HANDLE` as already-destroyed, AWP5) | Failure process / deciding which keys to destroy and when (WP6) | `aliro_ud_crypto::test_destroy_key_of_zero_is_a_no_op_success` (host + DK) | See `-018` DK run | ALIRO-SPEC §8.3.3.1 | app-implemented (key-destruction primitive only) |
| ALIRO-UD-SYRS-P1-039 | Reject malformed/unsupported-version/wrong-P1P2/out-of-sequence/incomplete-chained AUTH0/AUTH1 via Aliro failure process. | — | Command validation | — | — | ALIRO-SPEC §§8.3.3.1–4 | not-yet-verifiable |
| ALIRO-UD-SYRS-P1-040 | Meet every NFC/processing-time bound applicable to selected PICS in ALIRO-TP on target DK. | `src/platform/nfc/command_timing.h`/`.cpp` (`CommandTiming`, `BeginCommandTiming()`/`EndCommandTiming()`/`GetCommandTimingSnapshot()`/`ResetCommandTimingStats()`), `src/platform/nfc/nfc_worker.cpp` (wraps `HandleCommandApdu()`), `src/cli/cli.cpp` (`aliro-ud timing stats`/`reset`), `src/platform/nfc/nfc_transport.cpp` (`GetTimingConstraints()`, still `TimingConstraints{}`) | No normative numeric NFC processing-time bound applicable to this application's PICS was found in the corpus searched (AWP7); protocol-level ISO-DEP FWT is negotiated entirely inside `nfc_t4t_lib`, never surfaced to this application, with no application-facing WTX-request API | `aliro_nfc_user_device.functional.command_timing`/`command_timing_disabled` (16 host cases: `CommandTiming` logic + real worker/stack integration + disabled-build no-op) | DK build resource report (FLASH/RAM, AWP7); CLI `timing stats`/`reset` confirmed live on real hardware (no NFC reader available to collect an on-target sample) | ALIRO-SPEC Appendix 15; ALIRO-TP | not-yet-verifiable |

\* "zero or more EXCHANGE commands" per the SyRS wording.
