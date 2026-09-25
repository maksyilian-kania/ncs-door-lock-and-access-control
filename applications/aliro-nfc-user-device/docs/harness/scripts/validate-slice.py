#!/usr/bin/env python3
"""Validate the Aliro User Device slice catalog and one target slice."""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

SLICE_ID = re.compile(r"C\d+\.\d+")


def harness_root() -> Path:
    return Path(__file__).resolve().parents[1]


def frontmatter(path: Path) -> dict[str, str | list[str]]:
    lines = path.read_text(encoding="utf-8").splitlines()
    if not lines or lines[0] != "---":
        raise ValueError(f"{path}: missing YAML frontmatter")
    try:
        end = lines.index("---", 1)
    except ValueError as error:
        raise ValueError(f"{path}: unterminated YAML frontmatter") from error

    result: dict[str, str | list[str]] = {}
    key: str | None = None
    for line in lines[1:end]:
        if not line.strip() or line.lstrip().startswith("#"):
            continue
        item = re.fullmatch(r"\s+-\s+(.+?)\s*", line)
        if item:
            if key is None or not isinstance(result.get(key), list):
                raise ValueError(f"{path}: list item without a list key")
            result[key].append(item.group(1).strip("\"'"))
            continue
        field = re.fullmatch(r"([A-Za-z_][\w-]*):\s*(.*?)\s*", line)
        if not field:
            raise ValueError(f"{path}: unsupported frontmatter line: {line}")
        key, value = field.groups()
        if value == "[]":
            result[key] = []
        elif value:
            result[key] = value.strip("\"'")
        else:
            result[key] = []
    return result


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("target", help="exactly one slice ID, for example C2.1")
    args = parser.parse_args()
    if not SLICE_ID.fullmatch(args.target):
        parser.error("target must have the exact form Cx.y")

    slices_dir = harness_root() / "slices"
    errors: list[str] = []
    catalog: dict[str, tuple[Path, list[str]]] = {}

    for path in sorted(slices_dir.glob("*.md")):
        try:
            metadata = frontmatter(path)
            slice_id = metadata.get("id")
            prerequisites = metadata.get("prerequisites")
            if not isinstance(slice_id, str) or not SLICE_ID.fullmatch(slice_id):
                raise ValueError(f"{path}: invalid or missing id")
            if not path.name.startswith(f"{slice_id}-"):
                raise ValueError(f"{path}: filename does not begin with {slice_id}-")
            if not isinstance(prerequisites, list):
                raise ValueError(f"{path}: prerequisites must be a YAML list")
            if slice_id in catalog:
                raise ValueError(f"{path}: duplicate id {slice_id}")
            catalog[slice_id] = (path, prerequisites)
        except ValueError as error:
            errors.append(str(error))

    if not catalog:
        errors.append(f"{slices_dir}: no slice files found")
    for slice_id, (path, prerequisites) in catalog.items():
        for prerequisite in prerequisites:
            if not SLICE_ID.fullmatch(prerequisite):
                errors.append(f"{path}: invalid prerequisite {prerequisite}")
            elif prerequisite not in catalog:
                errors.append(f"{path}: unknown prerequisite {prerequisite}")
            elif prerequisite == slice_id:
                errors.append(f"{path}: slice cannot depend on itself")

    matches = [item for key, item in catalog.items() if key == args.target]
    if len(matches) != 1:
        errors.append(
            f"{args.target}: expected exactly one matching slice, found {len(matches)}"
        )
    if errors:
        print("\n".join(f"ERROR: {error}" for error in errors), file=sys.stderr)
        return 1

    path, prerequisites = matches[0]
    prerequisite_text = ", ".join(prerequisites) if prerequisites else "none"
    print(f"OK: {args.target} -> {path.relative_to(harness_root())}")
    print(f"Prerequisites (confirm independently): {prerequisite_text}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
