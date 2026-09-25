# Aliro User Device completion harness

This directory indexes the remaining application work for the
project-selected Aliro 1.0 NFC User Device profile.

`C` means **Compliance**. `C2` is compliance epic 2; `C2.1` is one
independently executable slice. Implementation requests must identify one
slice as `TARGET_SLICE=Cx.y`.

## Authority and scope

The Aliro 1.0 specification, queried through the configured Aliro
specification MCP, is normative. `../Requirements.pdf` is supplied locally
by the user, intentionally untracked, and selects this project's scope:

- Phase 1: `ALIRO-UD-SYRS-P1-001` through `P1-040`.
- Expedited Fast: `ALIRO-UD-SYRS-P2-001` through `P2-008`.
- NFC Step-Up: `ALIRO-UD-SYRS-P2-020` through `P2-032`.

BLE/UWB, extended-length APDUs, User Device Descriptor, NFC Step-Up AID
selection, notifications, and `update_doc` are not selected.

## Agent workflows

- `/ud-implement TARGET_SLICE=Cx.y` implements one slice and leaves changes
  uncommitted.
- `/ud-verify TARGET_SLICE=Cx.y` independently verifies one slice and commits
  it only when every required gate passes.

Both skills require explicit invocation.

The scripts under `scripts/` enforce harness structure:

- `validate-slice.py` runs at the start of both skills to validate the target,
  slice catalog, and prerequisite references.
- `validate-state.py` runs before either skill finishes to ensure local STATE
  is short, ignored, unstaged, and structurally valid.
- `validate-traceability.py` runs during verification: deferred mode before
  C1.4, then `--required` from C1.4 onward.

## Slice index

### C0 — Baseline

- [C0.1 — Baseline and public-contract mapping](slices/C0.1-baseline-contract.md)

### C1 — Phase 1 regression

- [C1.1 — Host-suite build repair](slices/C1.1-host-suite-build.md)
- [C1.2 — Standard authentication integration](slices/C1.2-standard-integration.md)
- [C1.3 — EXCHANGE and lifecycle integration](slices/C1.3-exchange-lifecycle.md)
- [C1.4 — Phase 1 evidence audit](slices/C1.4-phase1-evidence-audit.md)

### C2 — Durable `Kpersistent`

- [C2.1 — Backend semantics and direct tests](slices/C2.1-persistent-key-semantics.md)
- [C2.2 — Crash-safe persistence and recovery](slices/C2.2-persistent-key-recovery.md)
- [C2.3 — Persistence fault injection](slices/C2.3-persistent-key-fault-injection.md)

### C3 — Durable documents

- [C3.1 — Snapshot and persistence behavior](slices/C3.1-document-snapshots.md)
- [C3.2 — Provisioning validation](slices/C3.2-document-provisioning.md)

### C4 — Expedited Fast application integration

- [C4.1 — Fast backend readiness](slices/C4.1-fast-backend-readiness.md)
- [C4.2 — Standard-to-Fast integration](slices/C4.2-standard-to-fast.md)
- [C4.3 — Fast fallback and cleanup](slices/C4.3-fast-fallback-cleanup.md)
- [C4.4 — Fast EXCHANGE integration](slices/C4.4-fast-exchange.md)

### C5 — Access-only NFC Step-Up integration

- [C5.1 — Access Document provisioning readiness](slices/C5.1-access-document-readiness.md)
- [C5.2 — Step-Up transport integration](slices/C5.2-step-up-transport.md)
- [C5.3 — Step-Up lifecycle and cleanup](slices/C5.3-step-up-lifecycle.md)

### C6 — Stack-gated Step-Up completion

- [C6.1 — Updated-stack contract audit](slices/C6.1-stack-contract-audit.md)
- [C6.2 — Revocation integration](slices/C6.2-revocation-integration.md)
- [C6.3 — Combined and boundary integration](slices/C6.3-combined-boundaries.md)

### C7 — Target acceptance

- [C7.1 — Storage and crypto qualification](slices/C7.1-storage-crypto-target.md)
- [C7.2 — Standard and Fast Reader campaign](slices/C7.2-standard-fast-reader.md)
- [C7.3 — Step-Up Reader campaign](slices/C7.3-step-up-reader.md)
- [C7.4 — Release closure](slices/C7.4-release-closure.md)

## Dependency order

1. C0.1.
2. C1.1.
3. C1.2 and C1.3; then C1.4.
4. C2.1 → C2.2 → C2.3 and C3.1 → C3.2 may proceed independently
   after C1.1.
5. C4.1 → C4.2 → C4.3 → C4.4 requires C1.2, C1.3, and C2.3.
6. C5.1 → C5.2 → C5.3 requires C1.2 and C3.2.
7. C6.1 → C6.2 → C6.3 requires the upstream stack gate to be resolved.
8. C7 runs only after the corresponding host/integration slices pass.

## Current stack gate

C6 and complete C7 Step-Up acceptance are blocked until `ncs-aliro`
provides Revocation-only and combined responses, truthful signaling,
complete failure cleanup, and fixes for known deterministic-CBOR and
response-ordering defects.
