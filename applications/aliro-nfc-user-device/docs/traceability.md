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

**Post-AWP1 lifecycle-race hardening pass** (this fix): `platform/nfc`
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

| ID | Requirement (summary) | App files (this AWP) | Stack ownership | Tests | DK evidence | Normative citation | Status |
|---|---|---|---|---|---|---|---|
| ALIRO-UD-SYRS-P1-001 | Execute on nRF54LM20B DK + PCA64110 antenna. | `CMakeLists.txt`, `prj.conf`, `src/main.cpp`, `src/platform/nfc`, `src/platform/os` | — | `build.aliro_nfc_user_device` (twister, DK target build) | Flashed to a physical nRF54LM20 DK (serial 1051885995) and booted; console log shows stack init and NFC T4T listen mode start (see `docs/evidence.md`) | NORDIC-DK; project constraint | verified-end-to-end |
| ALIRO-UD-SYRS-P1-002 | Operate as NFC-A Type 4 Tag Platform / ISO-DEP listen mode. | `src/platform/nfc/nfc_transport.{h,cpp}`, `nfc_worker.{h,cpp}`, `apdu_fragment_assembler.{h,cpp}` | SELECT/ISO-DEP session behavior | `apdu_fragment_assembler` (fragment assembly/overflow/reset, host), `worker_lifecycle` (12 cases: idempotent field on/off, stack-driven timeout termination, duplicate/stale events, field-loss race, queue overflow forcing session recovery without dropping FIELD_OFF, APDU rejection with no active session, host, real stack) | NFC T4T listen mode confirmed started on DK console log (see `docs/evidence.md`); no physical reader/antenna tap performed yet | ALIRO-SPEC §10.1, p.93; NORDIC-NFC | app-implemented |
| ALIRO-UD-SYRS-P1-003 | Line-oriented CLI over DK virtual UART, 115200 8N1. | `src/cli` (skeleton only) | — | — | — | Project constraint; NORDIC-DK | not-yet-verifiable |
| ALIRO-UD-SYRS-P1-004 | CLI provisions Access Credential (key, bindings, trust, policy, timestamps, mailbox config, optional documents). | `src/storage/credential`, `src/cli` (skeletons only) | — | — | — | ALIRO-SPEC §§6.2, 8.3.1.14–15 | not-yet-verifiable |
| ALIRO-UD-SYRS-P1-005 | CLI create/update/inspect/delete/factory-reset with deterministic results. | `src/storage/credential`, `src/cli` (skeletons only) | — | — | — | Project constraint | not-yet-verifiable |
| ALIRO-UD-SYRS-P1-006 | Reject malformed provisioning input without changing committed state. | `src/storage/credential` (skeleton only) | — | — | — | Project constraint; security objective | not-yet-verifiable |
| ALIRO-UD-SYRS-P1-007 | Retain credentials/trust/policies/documents/mailbox/Kpersistent across reset and power loss. | `src/storage/credential` (skeleton only) | — | — | — | ALIRO-SPEC §§3.1, 6.2, 8.3.1.15 | not-yet-verifiable |
| ALIRO-UD-SYRS-P1-008 | Never export Access Credential private key, Kpersistent, or session key via CLI/NFC/logs. | `src/platform/crypto`, `src/storage/credential` (skeletons only) | Session key material | — | — | ALIRO-SPEC §16.2; NORDIC-SEC | not-yet-verifiable |
| ALIRO-UD-SYRS-P1-009 | Support at least 16 `reader_group_identifier` bindings per Access Credential. | `src/storage/credential` (skeleton only) | — | — | — | ALIRO-SPEC §6.2 | not-yet-verifiable |
| ALIRO-UD-SYRS-P1-010 | CLI method to select among multiple matching Access Credentials. | `src/storage/credential`, `src/cli` (skeletons only) | `Trust::GetReaderPublicKey/GetReaderIssuerPublicKey` binding-awareness (see AWP3 blocker note) | — | — | ALIRO-SPEC §6.2; ALIRO-TP Table 4-3 | not-yet-verifiable |
| ALIRO-UD-SYRS-P1-011 | DK button press = user authentication for a configurable 1–300 s window (default 30 s). | `src/platform/authorization` (skeleton only) | — | — | — | Project architecture; ALIRO-SPEC §8.3.1.14 | not-yet-verifiable |
| ALIRO-UD-SYRS-P1-012 | Visible indication when authentication is required and no valid window exists. | `src/platform/authorization` (skeleton only) | — | — | — | ALIRO-SPEC §8.3.1.14 | not-yet-verifiable |
| ALIRO-UD-SYRS-P1-013 | SELECT of AID `A000000909ACCE5501` returns conforming FCI (type `0x0000`, version `0x0100`, ≤256 bytes). | `src/platform/nfc` (transport wiring only; FCI bytes are stack-owned) | FCI/SELECT response encoding | `worker_lifecycle::test_select_expedited_aid_returns_spec_fci` (exact FCI bytes from the Aliro spec worked example, through the real worker + real stack, host) | Not yet exercised with a physical reader/antenna tap | ALIRO-SPEC §10.2.1.2 | verified-end-to-end (host); DK antenna tap pending |
| ALIRO-UD-SYRS-P1-014 | Encode expedited commands/responses as ISO/IEC 7816-4 APDUs per Aliro CLA/INS/P1/P2/Lc/Le/status words/TLV/endianness. | — | APDU/TLV encoding | — | — | ALIRO-SPEC §8.3.2.1 | not-yet-verifiable |
| ALIRO-UD-SYRS-P1-015 | Support command chaining for AUTH0 responses and LOAD CERT/AUTH1/EXCHANGE. | `src/platform/nfc/apdu_fragment_assembler.{h,cpp}` (raw ISO-DEP fragment reassembly only; Aliro-level command chaining is stack-owned) | Chaining state machine | `apdu_fragment_assembler::test_chained_fragments_assemble_in_order` and related cases (host) | — | ALIRO-SPEC §8.3.2.2; ALIRO-TP Table 4-3 | app-implemented (transport-level reassembly only) |
| ALIRO-UD-SYRS-P1-016 | Accept unknown extension TLVs where forward-compatible processing is required. | — | TLV parsing | — | — | ALIRO-SPEC §§8.3.3.2, 8.3.3.4, 10.2.1.2 | not-yet-verifiable |
| ALIRO-UD-SYRS-P1-017 | Support complete Expedited Standard flow: SELECT, AUTH0, optional LOAD CERT, AUTH1, EXCHANGE\*, result handling. | — | Session/Access Protocol state machine | — | — | ALIRO-SPEC §§8.1.1.1, 8.2 | not-yet-verifiable |
| ALIRO-UD-SYRS-P1-018 | Generate new random P-256 ephemeral Access Credential key pair per AUTH0 response. | `src/platform/crypto` (skeleton only) | AUTH0 response orchestration | — | — | ALIRO-SPEC §§8.3.3.2.6–7 | not-yet-verifiable |
| ALIRO-UD-SYRS-P1-019 | Select Access Credential/trust material using AUTH0 `reader_group_identifier`. | `src/storage/credential` (skeleton only) | AUTH0 parsing | — | — | ALIRO-SPEC §§6.2, 8.3.3.2.6 | not-yet-verifiable |
| ALIRO-UD-SYRS-P1-020 | Enforce `authentication_policy` values `0x01`/`0x02`/`0x03` per selected Access Credential policy. | `src/platform/authorization`, `src/storage/credential` (skeletons only) | Policy gate invocation | — | — | ALIRO-SPEC §8.3.1.14 | not-yet-verifiable |
| ALIRO-UD-SYRS-P1-021 | Policy `0x03` continues only with a valid button authorization window. | `src/platform/authorization` (skeleton only) | Policy gate invocation | — | — | ALIRO-SPEC §8.3.1.14; project architecture | not-yet-verifiable |
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
