---
name: ud-implement
description: Implement exactly one named Aliro NFC User Device application compliance slice. Use only when the user explicitly invokes this skill with TARGET_SLICE=Cx.y.
disable-model-invocation: true
---

# Aliro User Device slice implementation

## Required input

The invoking prompt must contain exactly one `TARGET_SLICE=Cx.y`.

If the target is absent, invalid, ambiguous, complete, blocked by an
incomplete prerequisite, or more than one target is supplied, stop and ask
the user. Never select or expand a slice autonomously.

## Start

1. Run
   `python3 applications/aliro-nfc-user-device/docs/harness/scripts/validate-slice.py TARGET_SLICE`.
   Confirm prerequisite completion from Git history and traceability/evidence,
   never from STATE alone.
2. Read:
   - `applications/aliro-nfc-user-device/docs/harness/STATE.md`;
   - the harness `README.md`;
   - only the selected slice file;
   - relevant passages in the user-supplied `docs/Requirements.pdf`.
3. Query the Aliro specification MCP for every protocol-sensitive question
   and retain its section/page citation.
4. Record:
   - application branch and HEAD;
   - exact `ncs-aliro` HEAD;
   - working-tree status.
5. Inspect current public `Aliro::Interface::UserDevice` headers before
   changing adapters or backends.

If STATE is absent, initialize it from `STATE.template.md`. If STATE
conflicts with Git/source, trust Git/source and correct STATE.

## Scope and ownership

- Implement only deliverables in the selected slice.
- Modify only application-owned code/tests in this repository.
- Preserve unrelated staged, unstaged, and untracked work.
- Never modify the sibling `ncs-aliro` checkout.
- Never implement APDU/TLV/CBOR processing, protocol state machines,
  chaining, cryptographic orchestration/KDF construction, counters, status
  words, policy, or Aliro failure sequencing in the application.
- Drive the real stack in integration tests only to verify the application
  boundary.
- If stack behavior is missing or defective, record the exact revision and
  blocker, leave protocol behavior unchanged, and stop that path.

## Implementation

1. Prefer Codegraph before reading/searching indexed source.
2. Implement the smallest cohesive application change satisfying the slice.
3. Add direct application-owned tests for every changed behavior.
4. Do not begin an adjacent slice or unrelated cleanup.
5. Use the `ncs4` shell function for every `west` command.

## Evidence

Create `docs/harness/evidence/Cx.y.md` only for durable information:

- a stack blocker with exact revision;
- a material specification interpretation;
- an unavailable required check;
- a fault-injection matrix;
- a hardware/external-harness result; or
- a non-obvious security conclusion.

Keep the eventual commit body concise: summarize what changed, why, and the
names and outcomes of required checks. Do not include command logs,
exhaustive symbol mappings, full test-case lists, or fault matrices.

Put durable detail needed after the commit—such as a public-contract mapping,
exact blocker analysis, or a non-trivial verification matrix—in the slice
evidence file. Transient command output belongs only in the agent report.

## Finish

1. Run every check required by the slice.
2. Run
   `python3 applications/aliro-nfc-user-device/docs/harness/scripts/validate-state.py`
   after refreshing local STATE when continuity-relevant context changed.
3. Check the diff for scope, secrets, and accidental stack-owned behavior.
4. Leave all implementation changes uncommitted.
5. Report:
   - files changed;
   - checks and exact results;
   - application defects fixed;
   - stack blockers;
   - remaining verification.
6. Stop. Never continue to another slice or commit.
