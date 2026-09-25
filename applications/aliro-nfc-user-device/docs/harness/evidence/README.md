# Harness evidence

Do not pre-create empty per-slice evidence files.

Create `Cx.y.md` only when a slice produces durable information that must
survive another clone, including:

- an exact stack blocker and tested revision;
- a material specification interpretation and citation;
- a public-contract mapping or other detailed audit result;
- an unavailable required check;
- a fault-injection matrix;
- a hardware or external-harness run; or
- a non-obvious security conclusion.

Commit bodies contain only a short what/why summary and compact required-check
outcomes. Do not put command logs, exhaustive mappings, full test-case lists,
or verification matrices in commit messages. Transient command output belongs
only in the agent report. Every C7 hardware slice must create an evidence
file.

The local `../STATE.md` is not evidence. Promote any durable conclusion from
STATE into this directory, traceability, a slice file, or a commit before it
is relied upon.
