#!/usr/bin/env python3
"""Validate the ignored local continuity STATE file."""

from __future__ import annotations

import re
import subprocess
import sys
from pathlib import Path


def repository_root(start: Path) -> Path:
    for candidate in (start, *start.parents):
        if (candidate / ".git").exists():
            return candidate
    raise RuntimeError("repository root not found")


def git(root: Path, *arguments: str) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        ["git", *arguments],
        cwd=root,
        check=False,
        capture_output=True,
        text=True,
    )


def main() -> int:
    state = Path(__file__).resolve().parents[1] / "STATE.md"
    root = repository_root(state)
    errors: list[str] = []

    if not state.is_file():
        print(f"ERROR: {state}: live STATE does not exist", file=sys.stderr)
        return 1

    lines = state.read_text(encoding="utf-8").splitlines()
    if len(lines) > 25:
        errors.append(f"STATE has {len(lines)} lines; maximum is 25")

    headings = [line for line in lines if re.match(r"^#{1,6}\s", line)]
    if headings != ["# Context", "## Current status"]:
        errors.append(
            "headings must be exactly '# Context' and '## Current status'"
        )

    try:
        status_start = lines.index("## Current status") + 1
    except ValueError:
        status_start = len(lines)
    status_bullets = [
        line for line in lines[status_start:] if re.match(r"^\s*-\s+\S", line)
    ]
    if len(status_bullets) > 6:
        errors.append(
            f"Current status has {len(status_bullets)} bullets; maximum is 6"
        )

    text = "\n".join(lines)
    if re.search(r"-----BEGIN [A-Z ]*(?:PRIVATE KEY|SECRET)-----", text):
        errors.append("STATE appears to contain private key or secret material")
    if re.search(r"(?<![A-Za-z0-9+/=])[A-Fa-f0-9]{64,}(?![A-Za-z0-9+/=])", text):
        errors.append("STATE contains a long hexadecimal token")
    if re.search(r"(?<![A-Za-z0-9+/=])[A-Za-z0-9+/]{80,}={0,2}(?![A-Za-z0-9+/=])", text):
        errors.append("STATE contains a long base64-like token")
    if any(len(line) > 240 for line in lines):
        errors.append("STATE contains a line longer than 240 characters")

    relative = state.relative_to(root)
    if git(root, "check-ignore", "-q", "--", str(relative)).returncode != 0:
        errors.append(f"{relative} is not ignored by Git")
    if (
        git(root, "diff", "--cached", "--quiet", "--", str(relative)).returncode
        != 0
    ):
        errors.append(f"{relative} is staged")

    if errors:
        print("\n".join(f"ERROR: {error}" for error in errors), file=sys.stderr)
        return 1
    print(f"OK: {relative} is a valid ignored continuity file")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
