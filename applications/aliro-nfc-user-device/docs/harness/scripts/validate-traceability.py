#!/usr/bin/env python3
"""Validate the intentionally narrow User Device traceability schema."""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

REQUIREMENT = re.compile(r"ALIRO-UD-SYRS-(P[12]-\d{3})")
SLICE = re.compile(r"C\d+\.\d+")


def selected_requirements() -> set[str]:
    return {
        *(f"ALIRO-UD-SYRS-P1-{number:03d}" for number in range(1, 41)),
        *(f"ALIRO-UD-SYRS-P2-{number:03d}" for number in range(1, 9)),
        *(f"ALIRO-UD-SYRS-P2-{number:03d}" for number in range(20, 33)),
    }


def field_section(block: list[str], field: str) -> tuple[str, list[str]]:
    prefix = f"  {field}:"
    for index, line in enumerate(block):
        if line.startswith(prefix):
            scalar = line[len(prefix) :].strip().strip("\"'")
            nested: list[str] = []
            for following in block[index + 1 :]:
                if re.match(r"^  [A-Za-z_][\w-]*:", following):
                    break
                nested.append(following)
            return scalar, nested
    return "", []


def list_values(scalar: str, nested: list[str]) -> list[str]:
    if scalar == "[]":
        return []
    if scalar.startswith("[") and scalar.endswith("]"):
        return [
            value.strip().strip("\"'")
            for value in scalar[1:-1].split(",")
            if value.strip()
        ]
    return [
        match.group(1).strip().strip("\"'")
        for line in nested
        if (match := re.match(r"^\s+-\s+(.+?)\s*$", line))
    ]


def verification_values(block: list[str], channel: str) -> list[str]:
    _, verification = field_section(block, "verification")
    prefix = f"    {channel}:"
    for index, line in enumerate(verification):
        if line.startswith(prefix):
            scalar = line[len(prefix) :].strip()
            nested: list[str] = []
            for following in verification[index + 1 :]:
                if re.match(r"^    [A-Za-z_][\w-]*:", following):
                    break
                nested.append(following)
            return list_values(scalar, nested)
    return []


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--required",
        action="store_true",
        help="fail when traceability.yaml has not yet been created",
    )
    args = parser.parse_args()

    harness = Path(__file__).resolve().parents[1]
    traceability = harness / "traceability.yaml"
    if not traceability.exists():
        if args.required:
            print("ERROR: traceability.yaml is required", file=sys.stderr)
            return 1
        print("OK: traceability.yaml is deferred until C1.4")
        return 0

    lines = traceability.read_text(encoding="utf-8").splitlines()
    starts = [
        index
        for index, line in enumerate(lines)
        if re.match(r"^-\s+requirement:\s+", line)
    ]
    blocks = [
        lines[start : starts[index + 1] if index + 1 < len(starts) else len(lines)]
        for index, start in enumerate(starts)
    ]
    errors: list[str] = []
    seen: dict[str, int] = {}
    known_slices = {
        match.group(1)
        for path in (harness / "slices").glob("*.md")
        if (match := re.match(r"(C\d+\.\d+)-", path.name))
    }

    for block in blocks:
        match = re.match(r"^-\s+requirement:\s+(.+?)\s*$", block[0])
        requirement = match.group(1).strip("\"'") if match else ""
        if not REQUIREMENT.fullmatch(requirement):
            errors.append(f"invalid requirement ID: {requirement or block[0]}")
            continue
        seen[requirement] = seen.get(requirement, 0) + 1

        slices_scalar, slices_nested = field_section(block, "application_slices")
        slices = list_values(slices_scalar, slices_nested)
        for slice_id in slices:
            if not SLICE.fullmatch(slice_id) or slice_id not in known_slices:
                errors.append(f"{requirement}: unknown slice {slice_id}")

        status, _ = field_section(block, "application_status")
        if status not in {"planned", "blocked-stack", "verified-host", "verified-target"}:
            errors.append(f"{requirement}: invalid application_status {status!r}")

        host = verification_values(block, "host")
        target = verification_values(block, "target")
        if status in {"verified-host", "verified-target"} and not host:
            errors.append(f"{requirement}: {status} requires host verification")
        if status == "verified-target" and not target:
            errors.append(f"{requirement}: verified-target requires target verification")

        gate, gate_nested = field_section(block, "stack_gate")
        if status == "blocked-stack" and gate == "none":
            errors.append(f"{requirement}: blocked-stack requires stack gate details")
        if gate != "none":
            gate_text = "\n".join(gate_nested)
            if not re.search(r"^\s+revision:\s+\S", gate_text, re.MULTILINE):
                errors.append(f"{requirement}: stack gate lacks revision")
            if not re.search(r"^\s+finding:\s+\S", gate_text, re.MULTILINE):
                errors.append(f"{requirement}: stack gate lacks finding")

        evidence_scalar, evidence_nested = field_section(block, "evidence")
        for evidence in list_values(evidence_scalar, evidence_nested):
            candidates = (harness / evidence, harness.parents[3] / evidence)
            if not any(candidate.exists() for candidate in candidates):
                errors.append(f"{requirement}: missing evidence path {evidence}")

    expected = selected_requirements()
    missing = sorted(expected - seen.keys())
    extras = sorted(seen.keys() - expected)
    duplicates = sorted(key for key, count in seen.items() if count != 1)
    if missing:
        errors.append(f"missing selected requirements: {', '.join(missing)}")
    if extras:
        errors.append(f"requirements outside selected scope: {', '.join(extras)}")
    if duplicates:
        errors.append(f"requirements not represented exactly once: {', '.join(duplicates)}")

    if errors:
        print("\n".join(f"ERROR: {error}" for error in errors), file=sys.stderr)
        return 1
    print(f"OK: {len(seen)} selected requirements are traceable exactly once")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
