# Aliro NFC User Device

Phase 1 Aliro **User Device** application for the nRF54LM20B DK, built on
the DK's integrated NFC-A antenna/NFCT peripheral and the checked-out
`Aliro::UserDeviceStack` facade (west project `ncs-aliro`). This replaces
the earlier System-OFF/wake-on-field proof of concept: Phase 1 (AWP0-AWP8)
implements the NFC-A Type 4 Tag/ISO-DEP listen transport and local
credential/trust/mailbox provisioning and button-based authorization
through a development CLI; a standalone System OFF feature
(`src/platform/power/`, not an AWP) was added on top afterward,
reintroducing NFC-field/Button-1 wake and Button-1/auto-idle sleep - see
`docs/system_off_proposal.md`.

See [`docs/architecture.md`](docs/architecture.md) for module boundaries and
the application/stack split, [`docs/traceability.md`](docs/traceability.md)
for per-requirement status, and [`docs/provisioning.md`](docs/provisioning.md)
for a full CLI provisioning walkthrough. `docs/evidence/AWP<n>.md` holds the
dated command/result log for each implementation package; `docs/STATE.md` is
the current checkpoint.

## Scope

Implemented by this application (Phase 1 only):

- NFC-A Type 4 Tag Platform / ISO-DEP listen transport, with a bounded
  queue and dedicated NFC/stack worker thread (`src/platform/nfc`).
- Zephyr execution primitives — mutex, timer, deferred-event queue, and a
  trusted-time stub that never asserts a value, since Phase 1 provisions no
  wall clock (`src/platform/os`).
- PSA/CRACEN/KMU-backed cryptography: random/ephemeral-key generation, raw
  ECDH, HKDF/HMAC-SHA-256 derivation, AES-GCM AEAD, ECDSA sign/verify,
  SHA-256, Aliro profile0000 Reader-certificate decompression/validation,
  and key de
v1.0.0-822cb32ce48d (￼https://github.com/nrfconnect/ncs-aliro/pull/727Your team sees richer Github previewsConnect )

￼
￼
struction (`src/platform/crypto`).
- Local button authorization and visible (LED) indication for
  `authentication_policy` values `0x01`/`0x02`/`0x03` (`src/platform/authorization`).
- Credential, per-reader-group trust, key, optional-document, and mailbox
  persistence over Zephyr settings/NVS and PSA/CRACEN/KMU, with a
  crash-safe provisioning transaction journal (`src/storage/credential`,
  `src/storage/mailbox`).
- A line-oriented development CLI over the DK's virtual UART for local
  provisioning, inspection, authorization test triggers, and diagnostics
  (`src/cli`).
- A lifecycle coordinator that serializes mutating CLI operations against
  any in-progress NFC session (`src/lifecycle`).
- System OFF: NFC-field and Button 1 wake, Button-1/auto-idle sleep via
  `sys_poweroff()` (`src/platform/power`; standalone feature, not an AWP -
  see `docs/system_off_proposal.md`).

Not implemented by this application — owned by the checked-out
`Aliro::UserDeviceStack` facade, per `APP_PLAN.md`'s boundary rules:

- Aliro APDU/TLV encoding, decoding, chaining, and status words.
- The Session and Access Protocol state machines: `SELECT`, `AUTH0`,
  `LOAD CERT`, `AUTH1`, `EXCHANGE`, and `CONTROL FLOW` behavior.
- Aliro cryptographic orchestration, KDF construction, and protocol
  sequencing (the application supplies only thin PSA primitives).

Explicitly excluded from Phase 1 (see `APP_PLAN.md` §5): BLE/UWB transports,
Reader/poll-mode behavior, extended-length APDUs, User Device Descriptor/
Reader-notification/`update_doc`, and all `ALIRO-UD-SYRS-P2-*` requirements.
System OFF is no longer excluded (see the Scope list above).

## Requirements

- nRF54LM20B DK (PCA10184) with the built-in NFC tag antenna.
- The `ncs-aliro` west project checked out alongside this repository (see
  `west-aliro.yml`), at or above the baseline revision recorded in
  `APP_PLAN.md` §1.
- An Aliro-capable NFC reader to exercise `SELECT`/`AUTH0`/`AUTH1`/`EXCHANGE`
  end to end (for example, the
  [Aliro Access Control Application](../aliro-access-control-app/)). No
  physical reader has been available in any AWP's development environment
  to date; see `docs/traceability.md` for which requirements are marked
  `not-yet-verifiable` as a result.
- A serial terminal for the DK's virtual UART (console + shell), 115200-8N1.

User device accepts only expedited standard phase. Reader app has to have expedited fast phase switched off.
Build with:

```bash
west build -p -b nrf54lm20dk/nrf54lm20b/cpuapp \
  applications/aliro-access-control-app \
  -- -DCONFIG_DOOR_LOCK_EXPEDITED_FAST_PHASE=n -DCONFIG_ST25R200_DRV=y
```

## Build and flash

From the west topdir, using the `ncs4` NCS v3.4.0 toolchain shell:

```bash
west build -p -b nrf54lm20dk/nrf54lm20a/cpuapp \
    ncs-door-lock-and-access-control.git/applications/aliro-nfc-user-device
west flash
```

A host-testable smoke build (native, no DK) is also available for every
module under `tests/functional/subsys/aliro_nfc_user_device/`:

```bash
west twister -T ncs-door-lock-and-access-control.git/tests/functional/subsys/aliro_nfc_user_device \
    -p native_sim/native/64
```

## Development CLI

Connect a serial terminal to the DK's virtual UART at 115200-8N1 and use the
`aliro-ud` root shell command. Every command returns exactly one
deterministic `OK ...`/`ERR ...` line and never prints secret values
(private keys, `Kpersistent`, or session keys).

```
aliro-ud info                                   # non-secret build/session state
aliro-ud credential begin-create|begin-update <handle>|set-key|set-binding|
                    set-policy|set-mailbox|set-mailbox-data-subset|
                    set-credential-timestamp|set-revocation-timestamp|
                    set-access-document|set-revocation-document|
                    commit|abort
aliro-ud credential list|inspect <handle>|delete <handle>|reset|
                    bindings <handle>|preferred-set|preferred-get
aliro-ud mailbox inspect|read|init|reset <handle> ...
aliro-ud auth status|press|clear|notify-required
aliro-ud timing stats|reset
```

See [`docs/provisioning.md`](docs/provisioning.md) for the full staging
transaction workflow (create → set fields → commit) and example
deterministic output for every command above.

## Current status

Every Phase 1 requirement (`ALIRO-UD-SYRS-P1-001` through `-040`) is tracked
in [`docs/traceability.md`](docs/traceability.md). In summary:

- Application-owned transport, CLI, credential/trust/mailbox persistence,
  authorization, and cryptography are implemented and host-tested; most
  have also been demonstrated on a physical nRF54LM20 DK over the shell UART
  (see the per-AWP evidence files for exact sessions).
- No physical Aliro NFC reader has been available in any development
  environment used so far, so no AWP has demonstrated a real Reader-driven
  `AUTH0`/`AUTH1`/`EXCHANGE` transaction on-target; those requirements are
  `not-yet-verifiable` pending reader access.
- `ALIRO-UD-SYRS-P1-040` (NFC timing bounds) has no applicable numeric bound
  in the searched Aliro 1.0 Specification and Test Plan corpus for this
  application's NFC-only PICS; see `docs/evidence/AWP7.md`.
- `worker_lifecycle`, `authorization`, and `cli_info` currently fail to
  *link* against the checked-out `ncs-aliro` dev revision on pre-existing
  `Crypto`/`CredentialSigning` symbols — an external stack issue, out of
  scope for this application; see `docs/evidence/AWP7.md`'s "External stack
  observations".

## Source layout

```
applications/aliro-nfc-user-device/
├── CMakeLists.txt, Kconfig, prj.conf, sample.yaml
├── boards/                          # board overlay/conf
├── docs/
│   ├── architecture.md              # module boundaries, application/stack split
│   ├── provisioning.md              # CLI provisioning workflow and example output
│   ├── traceability.md              # per-requirement status
│   ├── wp7_stack_impact.md          # out-of-band stack-breaking-change fix log
│   ├── STATE.md                     # current AWP checkpoint
│   └── evidence/AWP<n>.md           # one dated command/result log per AWP
└── src/
    ├── main.cpp                     # boot sequencing only
    ├── lifecycle/                   # mutating-operation coordinator
    ├── platform/{nfc,os,crypto,authorization,power}/
    ├── storage/{credential,mailbox}/
    └── cli/                         # aliro-ud shell command tree
```

Host tests for every module live under
`tests/functional/subsys/aliro_nfc_user_device/` at the repository root
(`apdu_fragment_assembler`, `authorization`, `cli_info`, `command_timing`,
`crypto`, `host_smoke`, `mailbox`, `power`, `worker_lifecycle`).

## References

- Aliro 1.0 Specification and Test Plan (queried through the `aliro-spec`
  MCP; normative citations are recorded beside each traceability row and
  design decision).
- `applications/aliro-nfc-user-device/docs/Requirements.pdf` —
  `ALIRO-UD-SYRS-P1-001` through `-040` (untracked in this repository by
  explicit user instruction; obtain locally if needed).
- `APP_PLAN.md` — the work-package plan this application was built against.
- Reader reference in this repository: `applications/aliro-access-control-app/`.
