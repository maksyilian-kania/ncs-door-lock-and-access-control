---
name: ud-verify
description: Independently verify and, only on full success, commit one named Aliro NFC User Device compliance slice. Use only when explicitly invoked with TARGET_SLICE=Cx.y.
disable-model-invocation: true
---

# Aliro User Device slice verification and commit

Explicit invocation of this skill authorizes a commit only when every gate
below passes for exactly one `TARGET_SLICE=Cx.y`.

## Establish scope

1. Run
   `python3 applications/aliro-nfc-user-device/docs/harness/scripts/validate-slice.py TARGET_SLICE`.
2. Read local STATE, the harness index, the selected slice, relevant local
   Requirements passages, and the complete working-tree diff.
3. Query the Aliro specification MCP for each protocol-sensitive acceptance
   decision and retain section/page citations.
4. Record exact application and `ncs-aliro` revisions.
5. Confirm prerequisites are complete from Git history/evidence, not merely
   asserted by STATE.
6. Identify every file belonging to the slice. Preserve all unrelated work.

If target, scope, prerequisites, ownership, or required results are
ambiguous, do not commit.

## Independent review

- Confirm every deliverable and exit criterion in the slice.
- Confirm every changed behavior is application-owned.
- Reject changes that modify `ncs-aliro` or duplicate, replace, patch around,
  or compensate for stack-owned protocol behavior.
- Confirm the local Requirements PDF and normative Aliro specification both
  support the implementation.
- Review sensitive output for keys, secrets, raw provisioning material, and
  excessive protocol data.
- For storage/cryptographic boundary changes, run the security-review skill
  when the selected slice requires it.
- Do not repair substantive defects under this skill. Report failure and
  return the slice to implementation.

## Execute verification

1. Run every check listed in the selected slice; do not substitute weaker
   checks.
2. Use the `ncs4` shell function for every `west` command.
3. Run relevant regression tests and static/lint checks.
4. Validate STATE with
   `python3 applications/aliro-nfc-user-device/docs/harness/scripts/validate-state.py`.
5. From C1.4 onward, validate traceability with
   `python3 applications/aliro-nfc-user-device/docs/harness/scripts/validate-traceability.py --required`;
   before C1.4 omit `--required`.
6. Validate referenced evidence and exact stack revisions.

An unavailable, skipped, flaky, or failed required check blocks the commit.
Record durable blockers or unavailable checks in
`docs/harness/evidence/Cx.y.md` when useful, and keep STATE concise.

## Commit gate

Commit only if:

- all requirements, deliverables, checks, and exit criteria pass;
- no unresolved application defect or stack blocker affects the slice;
- the diff contains no unrelated files or stack-owned behavior;
- evidence and traceability are valid where required; and
- STATE is valid, ignored, and unstaged.

Then:

1. Stage only files belonging to this slice. Never stage STATE,
   `docs/Requirements.pdf`, `compile_commands.json`, unrelated manifest
   changes, or unrelated user work.
2. Reinspect the staged diff.
3. Create one signed-off commit beginning `aliro_ud: Complete Cx.y`. Keep its
   body to a short what/why summary and compact required-check outcomes. Do
   not include command logs, exhaustive mappings, full test-case lists, or
   verification matrices; durable detail belongs in slice evidence.
4. Update ignored STATE with the result and commit hash.
5. Report the commit, checks, evidence, and any follow-up slice now
   unblocked.
6. Stop after this one slice.

If any gate fails, do not stage or commit.
