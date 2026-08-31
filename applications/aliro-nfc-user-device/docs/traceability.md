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

| ID | Requirement (summary) | App files (this AWP) | Stack ownership | Tests | DK evidence | Normative citation | Status |
|---|---|---|---|---|---|---|---|
| ALIRO-UD-SYRS-P1-001 | Execute on nRF54LM20B DK + PCA64110 antenna. | `CMakeLists.txt`, `prj.conf`, `src/main.cpp`, `src/platform/nfc`, `src/platform/os` | — | `build.aliro_nfc_user_device` (twister, DK target build) | Flashed to a physical nRF54LM20 DK (serial 1051885995) and booted; console log shows stack init and NFC T4T listen mode start (see `docs/evidence.md`) | NORDIC-DK; project constraint | verified-end-to-end |
| ALIRO-UD-SYRS-P1-002 | Operate as NFC-A Type 4 Tag Platform / ISO-DEP listen mode. | `src/platform/nfc/nfc_transport.{h,cpp}`, `nfc_worker.{h,cpp}`, `apdu_fragment_assembler.{h,cpp}` | SELECT/ISO-DEP session behavior | `apdu_fragment_assembler` (fragment assembly/overflow/reset, host), `worker_lifecycle` (12 cases: idempotent field on/off, stack-driven timeout termination, duplicate/stale events, field-loss race, queue overflow forcing session recovery without dropping FIELD_OFF, APDU rejection with no active session, host, real stack) | NFC T4T listen mode confirmed started on DK console log (see `docs/evidence.md`); no physical reader/antenna tap performed yet | ALIRO-SPEC §10.1, p.93; NORDIC-NFC | app-implemented |
| ALIRO-UD-SYRS-P1-003 | Line-oriented CLI over DK virtual UART, 115200 8N1. | `src/cli/cli.cpp`, `src/platform/os/app_status.{h,cpp}`, `prj.conf` (`CONFIG_SHELL=y`) | — | `cli_info` (3 cases: `info` fields/format, live session-state reflection, credential-shell `ERR NOT_IMPLEMENTED` responses; host, native shell/dummy backend, real stack) | Flashed to physical nRF54LM20 DK (serial 1051885995) and driven over the shell virtual COM port at 115200-8N1: `aliro-ud info`, `credential begin-create/set-key/commit/abort` (all deterministic `OK`/`ERR NOT_IMPLEMENTED` lines), and command-tree/`help` listing (see `docs/evidence.md`) | Project constraint; NORDIC-DK | verified-end-to-end |
| ALIRO-UD-SYRS-P1-004 | CLI provisions Access Credential (key, bindings, trust, policy, timestamps, mailbox config, optional documents). | `src/storage/credential`, `src/cli/cli.cpp` (staging transaction + `commit`) | — | `cli_info::test_credential_staging_transaction_commits` (host, real stack) | DK build passed (commit "Implement AWP3: credential and trust persistence"); no recorded interactive DK provisioning demonstration yet (evidence gap left by that commit, see `docs/evidence.md`'s "Resolution" note) | ALIRO-SPEC §§6.2, 8.3.1.14–15 | app-implemented |
| ALIRO-UD-SYRS-P1-005 | CLI create/update/inspect/delete/factory-reset with deterministic results. | `src/storage/credential/credential_store.{h,cpp}`, `src/cli/cli.cpp` | — | `cli_info::test_credential_staging_transaction_commits` (create/inspect/list/delete, host, real stack) | DK build passed; no recorded interactive DK demonstration yet (see above) | Project constraint | app-implemented |
| ALIRO-UD-SYRS-P1-006 | Reject malformed provisioning input without changing committed state. | `src/storage/credential/credential_store.cpp` (`ValidatePayloadShape()`, validate-before-commit) | `CredentialManager::Create/Update()` validate-before-commit (WP5.5, D7) | `cli_info` host suite (host, real stack) | — | Project constraint; security objective | app-implemented |
| ALIRO-UD-SYRS-P1-007 | Retain credentials/trust/policies/documents/mailbox/Kpersistent across reset and power loss. | `src/storage/credential/{credential_store,credential_persistence_settings,key_backend_psa}.cpp` (Zephyr settings/NVS + PSA/CRACEN/KMU, 4-phase journal) | — | Journal/persistence logic implemented; no host or DK fault-injection/reset/power-loss test evidence recorded for this behavior yet (evidence gap, see `docs/evidence.md`'s "Resolution" note) | DK build passed; no recorded interactive persistence demonstration yet | ALIRO-SPEC §§3.1, 6.2, 8.3.1.15 | app-implemented |
| ALIRO-UD-SYRS-P1-008 | Never export Access Credential private key, Kpersistent, or session key via CLI/NFC/logs. | `src/platform/crypto` (skeleton only, AWP5), `src/storage/credential` (opaque PSA key IDs only, never a raw scalar) | Session key material | AWP1-4 "Security check" grep review of every changed file each AWP (see `docs/evidence.md`) | — | ALIRO-SPEC §16.2; NORDIC-SEC | app-implemented (credential-storage portion; session-key portion is AWP5/AWP6) |
| ALIRO-UD-SYRS-P1-009 | Support at least 16 `reader_group_identifier` bindings per Access Credential. | `src/storage/credential/credential_types.h` (`kMaxBindingsPerCredential`, default 16), `credential_store.cpp` | — | No dedicated 16-binding-capacity test recorded yet (evidence gap, see `docs/evidence.md`'s "Resolution" note) | — | ALIRO-SPEC §6.2 | app-implemented |
| ALIRO-UD-SYRS-P1-010 | CLI method to select among multiple matching Access Credentials. | `src/storage/credential/credential_store.cpp` (`SetPreferredCredential`/`GetPreferredCredential`/`ResolveByReaderGroupIdentifier`), `src/cli/cli.cpp` (`credential preferred-set/preferred-get`) | `Trust::GetReaderPublicKey/GetReaderIssuerPublicKey` now binding-aware (`ncs-aliro` WP5.5, decision D9 — resolves the AWP3 blocker note) | No dedicated preferred-selection test recorded yet (evidence gap, see `docs/evidence.md`'s "Resolution" note) | — | ALIRO-SPEC §6.2; ALIRO-TP Table 4-3 | app-implemented |
| ALIRO-UD-SYRS-P1-011 | DK button press = user authentication for a configurable 1–300 s window (default 30 s). | `src/platform/authorization/{authorization_window,authorization,authorization_button}.{h,cpp}`, `Kconfig` (`CONFIG_ALIRO_UD_AUTHORIZATION_WINDOW_SECONDS`, range 1-300, default 30) | — | `authorization` suite (17 cases: window boundaries/retry/preauthorization with a fake monotonic clock, contract-level `GetState()`, a real-AUTH0-through-real-stack e2e case; host) | Performed: real presses of DK `Button 0` opened, re-extended, and expired the window, cross-checked against `aliro-ud auth status` over the shell UART (see `docs/evidence.md` AWP4 entry, which also documents an on-target main-thread stack-overflow fix required to get the shell UART working) | Project architecture; ALIRO-SPEC §8.3.1.14 | verified-end-to-end |
| ALIRO-UD-SYRS-P1-012 | Visible indication when authentication is required and no valid window exists. | `src/platform/authorization/{authorization_indicator.h,authorization_led,authorization}.cpp` | — | `authorization` suite: `NotifyAuthenticationRequired()`/indicator-activation cases (host) | Performed via the new `aliro-ud auth notify-required` CLI test trigger (no NFC reader available to drive AUTH0 end to end): LED0 lit then cleared by a real button press, confirmed by a debug-probe GPIO register read and the user's visual observation (see `docs/evidence.md` AWP4 entry) | ALIRO-SPEC §8.3.1.14 | verified-end-to-end |
| ALIRO-UD-SYRS-P1-013 | SELECT of AID `A000000909ACCE5501` returns conforming FCI (type `0x0000`, version `0x0100`, ≤256 bytes). | `src/platform/nfc` (transport wiring only; FCI bytes are stack-owned) | FCI/SELECT response encoding | `worker_lifecycle::test_select_expedited_aid_returns_spec_fci` (exact FCI bytes from the Aliro spec worked example, through the real worker + real stack, host) | Not yet exercised with a physical reader/antenna tap | ALIRO-SPEC §10.2.1.2 | verified-end-to-end (host); DK antenna tap pending |
| ALIRO-UD-SYRS-P1-014 | Encode expedited commands/responses as ISO/IEC 7816-4 APDUs per Aliro CLA/INS/P1/P2/Lc/Le/status words/TLV/endianness. | — | APDU/TLV encoding | — | — | ALIRO-SPEC §8.3.2.1 | not-yet-verifiable |
| ALIRO-UD-SYRS-P1-015 | Support command chaining for AUTH0 responses and LOAD CERT/AUTH1/EXCHANGE. | `src/platform/nfc/apdu_fragment_assembler.{h,cpp}` (raw ISO-DEP fragment reassembly only; Aliro-level command chaining is stack-owned) | Chaining state machine | `apdu_fragment_assembler::test_chained_fragments_assemble_in_order` and related cases (host) | — | ALIRO-SPEC §8.3.2.2; ALIRO-TP Table 4-3 | app-implemented (transport-level reassembly only) |
| ALIRO-UD-SYRS-P1-016 | Accept unknown extension TLVs where forward-compatible processing is required. | — | TLV parsing | — | — | ALIRO-SPEC §§8.3.3.2, 8.3.3.4, 10.2.1.2 | not-yet-verifiable |
| ALIRO-UD-SYRS-P1-017 | Support complete Expedited Standard flow: SELECT, AUTH0, optional LOAD CERT, AUTH1, EXCHANGE\*, result handling. | — | Session/Access Protocol state machine | — | — | ALIRO-SPEC §§8.1.1.1, 8.2 | not-yet-verifiable |
| ALIRO-UD-SYRS-P1-018 | Generate new random P-256 ephemeral Access Credential key pair per AUTH0 response. | `src/platform/crypto` (skeleton only) | AUTH0 response orchestration | — | — | ALIRO-SPEC §§8.3.3.2.6–7 | not-yet-verifiable |
| ALIRO-UD-SYRS-P1-019 | Select Access Credential/trust material using AUTH0 `reader_group_identifier`. | `src/storage/credential/credential_store.cpp` (`ResolveByReaderGroupIdentifier`, `GetReaderPublicKey`/`GetReaderIssuerPublicKey`) | AUTH0 parsing, `CredentialManager::EvaluateAuth0()`/`ResolveReaderTrust()` orchestration | `authorization_e2e` (real AUTH0 -> real `CredentialManager` -> real `credential_store` lookup, host) | — | ALIRO-SPEC §§6.2, 8.3.3.2.6 | app-implemented |
| ALIRO-UD-SYRS-P1-020 | Enforce `authentication_policy` values `0x01`/`0x02`/`0x03` per selected Access Credential policy. | `src/platform/authorization/{authorization_window,authorization}.{h,cpp}` (uniform window gate; `GetState()` has no policy parameter, so combining the Reader-requested and credential-provisioned policy values is entirely stack-owned) | `CredentialManager::EvaluateAuth0()` policy combination (stack's own `test_policy.cpp` covers 0x01/0x02/0x03 directly) | `authorization_e2e::test_auth0_policy_0x03_*` (application portion, host, real stack) | Not yet performed: the DK demonstration available this AWP (see `docs/evidence.md`) exercised the button/LED window mechanics via CLI test triggers, not a real AUTH0 exchange (no NFC reader in this environment) — policy-value combination itself is unverified on-target | ALIRO-SPEC §8.3.1.14 | app-implemented |
| ALIRO-UD-SYRS-P1-021 | Policy `0x03` continues only with a valid button authorization window. | `src/platform/authorization/{authorization_window,authorization}.{h,cpp}` | Policy gate invocation (`CredentialManager::EvaluateAuth0()`) | `authorization` suite: `test_auth0_policy_0x03_with_no_window_indicates_required`/`test_auth0_policy_0x03_with_open_window_does_not_indicate` (host, real stack) | Not yet performed: same limitation as `-020` above — no NFC reader in this environment to drive a real AUTH0 exchange on-target | ALIRO-SPEC §8.3.1.14; project architecture | app-implemented |
| ALIRO-UD-SYRS-P1-022 | No-certificate case: authenticate Reader via AUTH1 signature with bound `reader_PubK`. | `src/storage/credential` (trust lookup, skeleton only) | AUTH1 signature verification call | — | — | ALIRO-SPEC §8.3.3.4.5 | not-yet-verifiable |
| ALIRO-UD-SYRS-P1-023 | Certificate case: authenticate Reader via certificate subject key only after issuer-CA verification. | `src/platform/crypto`, `src/storage/credential` (skeletons only) | LOAD CERT/AUTH1 orchestration | — | — | ALIRO-SPEC §§6.3.1, 8.3.3.3, 8.3.3.4.5 | not-yet-verifiable |
| ALIRO-UD-SYRS-P1-024 | Reject invalid/untrusted Reader certificate without disclosing trust-material existence. | `src/platform/crypto` (skeleton only) | Certificate/session failure handling | — | — | ALIRO-SPEC §§8.3.3.3.4, 8.3.3.4.5–6 | not-yet-verifiable |
| ALIRO-UD-SYRS-P1-025 | Derive Kdh/ExpeditedSKReader/ExpeditedSKDevice/StepUpSK via ECKA-DH + HKDF-HMAC-SHA-256 (byte `0x5E`). | `src/platform/crypto` (thin PSA bindings, skeleton only) | KDF construction/sequencing | — | — | ALIRO-SPEC §§8.3.1.4–5, 8.3.1.13 | not-yet-verifiable |
| ALIRO-UD-SYRS-P1-026 | Protect AUTH1/EXCHANGE responses with AES-GCM, direction-specific keys, session counter. | `src/platform/crypto` (skeleton only) | AEAD orchestration/counters | — | — | ALIRO-SPEC §§8.3.1.6–11, 8.3.3.4.6, 8.3.3.5.5 | not-yet-verifiable |
| ALIRO-UD-SYRS-P1-027 | Generate 64-byte ECDSA P-256/SHA-256 User Device signature over exact AUTH1 fields using the Access Credential private key. | `src/platform/crypto`, `src/storage/credential` (opaque-handle signing, skeletons only) | AUTH1 field assembly | — | — | ALIRO-SPEC §§8.3.1.2, 8.3.3.4.3, 8.3.3.4.6 | not-yet-verifiable |
| ALIRO-UD-SYRS-P1-028 | Return Access Credential public key or `key_slot` per AUTH1 parameter. | `src/storage/credential` (skeleton only) | AUTH1 response assembly | — | — | ALIRO-SPEC §8.3.3.4.2 | not-yet-verifiable |
| ALIRO-UD-SYRS-P1-029 | Return accurate `signaling_bitmap` (documents, mailbox state/rights, optional EXCHANGE capabilities). | `src/storage/credential`, `src/storage/mailbox` (skeletons only) | AUTH1 response assembly | — | — | ALIRO-SPEC §8.3.3.4.2 | not-yet-verifiable |
| ALIRO-UD-SYRS-P1-030 | Support `credential_signed_timestamp`/`revocation_signed_timestamp` in AUTH1 response when provisioned. | `src/storage/credential` (skeleton only) | AUTH1 response assembly | — | — | ALIRO-SPEC §8.3.3.4.2; ALIRO-TP Table 4-3 | not-yet-verifiable |
| ALIRO-UD-SYRS-P1-031 | AUTH0/AUTH1 externally observable data independent of credential/reader-key/Kpersistent existence, except protocol-defined result. | `src/storage/credential`, `src/platform/crypto` (skeletons only) | AUTH0/AUTH1 response shaping | — | — | ALIRO-SPEC §§8.3.3.2.6, 8.3.3.4.6 | not-yet-verifiable |
| ALIRO-UD-SYRS-P1-032 | Persistent mailbox per Access Credential with provisionable size, read/write permission, optional AUTH1 subset. | `src/storage/mailbox` (skeleton only) | — | — | — | ALIRO-SPEC §8.3.1.15 | not-yet-verifiable |
| ALIRO-UD-SYRS-P1-033 | Execute authorized mailbox read/write/set at reader-selected offsets through EXCHANGE. | `src/storage/mailbox` (skeleton only) | EXCHANGE parsing/orchestration | — | — | ALIRO-SPEC §§8.3.1.15, 8.3.3.5 | not-yet-verifiable |
| ALIRO-UD-SYRS-P1-034 | Commit all write/set requests in one EXCHANGE atomically; preserve pre-session data until close. | `src/storage/mailbox` (skeleton only) | EXCHANGE session boundary | — | — | ALIRO-SPEC §§8.3.1.15, 8.3.3.5.4 | not-yet-verifiable |
| ALIRO-UD-SYRS-P1-035 | Return Aliro secure-channel error sequence on unauthorized/out-of-bounds/malformed/failed EXCHANGE. | `src/storage/mailbox` (skeleton only) | Secure-channel error sequencing | — | — | ALIRO-SPEC §8.3.3.5.5 | not-yet-verifiable |
| ALIRO-UD-SYRS-P1-036 | Return secure-channel success sequence only after all EXCHANGE requests succeed. | `src/storage/mailbox` (skeleton only) | Secure-channel success sequencing | — | — | ALIRO-SPEC §8.3.3.5.5 | not-yet-verifiable |
| ALIRO-UD-SYRS-P1-037 | Process final Reader status in EXCHANGE / unencrypted CONTROL FLOW failure and terminate transaction. | `src/platform/nfc` (skeleton only) | Transaction termination sequencing | — | — | ALIRO-SPEC §§8.3.3.5, 10.2–10.2.2 | not-yet-verifiable |
| ALIRO-UD-SYRS-P1-038 | On Aliro failure: empty response data, applicable error status, destroy session-bound keys/data, terminate transaction. | `src/platform/nfc`, `src/platform/crypto` (skeletons only) | Failure process | — | — | ALIRO-SPEC §8.3.3.1 | not-yet-verifiable |
| ALIRO-UD-SYRS-P1-039 | Reject malformed/unsupported-version/wrong-P1P2/out-of-sequence/incomplete-chained AUTH0/AUTH1 via Aliro failure process. | — | Command validation | — | — | ALIRO-SPEC §§8.3.3.1–4 | not-yet-verifiable |
| ALIRO-UD-SYRS-P1-040 | Meet every NFC/processing-time bound applicable to selected PICS in ALIRO-TP on target DK. | (instrumentation added in AWP7) | Protocol timing | — | — | ALIRO-SPEC Appendix 15; ALIRO-TP | not-yet-verifiable |

\* "zero or more EXCHANGE commands" per the SyRS wording.
