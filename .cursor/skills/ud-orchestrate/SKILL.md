---
name: ud-orchestrate
description: Sequentially orchestrate explicitly listed Aliro NFC User Device slices using fresh implementation and verification subagents with user-selected models. Use only when explicitly invoked with SCOPE, IMPLEMENTATION_LLM, and VERIFICATION_LLM.
disable-model-invocation: true
---

# Aliro User Device slice orchestration

## Required input

The invoking prompt must provide all three parameters:

```text
SCOPE=C1.1,C1.2,C1.3
IMPLEMENTATION_LLM=<supported-model-id-or-inherit>
VERIFICATION_LLM=<supported-model-id-or-inherit>
```

`SCOPE` is an ordered, comma-separated list of explicit slice IDs. A count,
range, epic name, `next`, or other implicit scope is invalid. If any parameter
is absent or ambiguous, stop and ask the user.

Validate both model IDs against the models currently available for subagents.
Never guess, substitute, or silently fall back to another model. `inherit` is
valid only when the user supplies it explicitly.

## Validate scope

1. Read the harness README and local STATE.
2. Split `SCOPE` without reordering it. Reject duplicates.
3. Run `validate-slice.py` for every listed ID.
4. Confirm each prerequisite is already complete or appears earlier in
   `SCOPE`.
5. Reject completed, unknown, ambiguous, or blocked slices and ask the user.
6. Record the initial HEAD and working-tree state; preserve unrelated work.

Never add, remove, reorder, or substitute slices. One orchestration invocation
processes only the supplied `SCOPE`.

## Process each slice

Run slices sequentially. Never run writing subagents in parallel.

### 1. Implementation

Launch a fresh local foreground general-purpose subagent using
`IMPLEMENTATION_LLM`. Its prompt must:

- identify the repository and exact `TARGET_SLICE=Cx.y`;
- explicitly instruct it to follow
  `.cursor/skills/ud-implement/SKILL.md`;
- tell it to read the harness STATE, index, selected slice, requirements, and
  project rules;
- require application-owned changes only, all slice checks, a concise
  structured handoff, and no commit; and
- identify unrelated pre-existing work that must remain untouched.

Wait for completion. Confirm it did not commit or modify unrelated/stack-owned
files. If it fails, reports a blocker, broadens scope, or leaves an ambiguous
result, stop the entire orchestration.

### 2. Independent verification

Launch a new local foreground general-purpose subagent using
`VERIFICATION_LLM`. Do not resume or reuse the implementer. Its prompt must:

- identify the repository and exact `TARGET_SLICE=Cx.y`;
- explicitly instruct it to follow `.cursor/skills/ud-verify/SKILL.md`;
- include the implementer's concise handoff and identify pre-existing
  unrelated work;
- require independent inspection of the current diff and every slice gate;
  and
- authorize only the one signed-off slice commit allowed by the verification
  skill.

Wait for completion. Confirm verification passed, exactly one expected commit
was created, its title begins `aliro_ud: Complete Cx.y`, STATE was updated,
and unrelated work remains untouched. Otherwise stop the entire orchestration.

### 3. Continue

Reread STATE and validate the working tree before starting the next listed
slice. Never continue after a failed, unavailable, skipped, or ambiguous
verification.

## Finish

After all listed slices succeed, report:

- each slice and commit hash;
- implementation and verification model used;
- required-check outcomes;
- evidence created;
- newly unblocked slices; and
- unrelated work left untouched.

Keep handoffs and the final report concise. Detailed durable results belong
in slice evidence, not orchestration output or commit messages.
