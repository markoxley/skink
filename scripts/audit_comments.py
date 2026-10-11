#!/usr/bin/env python3
"""Find Skink/C functions and structs that are missing a leading docstring."""

import re
import sys
from pathlib import Path

REPO = Path("/home/mark/Documents/GitHub/skink")

SKINK_FN_RE = re.compile(r"^(pub\s+)?fn\s+(\w+)")
SKINK_STRUCT_RE = re.compile(r"^(pub\s+)?struct\s+(\w+)")
C_FN_RE = re.compile(r"^[A-Za-z_][A-Za-z0-9_]*\s+[A-Za-z_][A-Za-z0-9_]*\s*\(")


def is_comment(line: str, ch: str) -> bool:
    return line.strip().startswith(ch)


def has_leading_doc(lines: list[str], idx: int, ch: str) -> bool:
    """Check whether the first non-blank line before `idx` is a comment."""
    i = idx - 1
    while i >= 0 and lines[i].strip() == "":
        i -= 1
    return i >= 0 and is_comment(lines[i], ch)


def audit_skink(path: Path) -> list[tuple[int, str, str]]:
    text = path.read_text(encoding="utf-8", errors="replace")
    lines = text.splitlines()
    missing = []
    for i, line in enumerate(lines):
        # Functions
        m = SKINK_FN_RE.match(line)
        if m:
            name = m.group(2)
            if not has_leading_doc(lines, i, "//"):
                missing.append((i + 1, "fn", name))
            continue
        m = SKINK_STRUCT_RE.match(line)
        if m:
            name = m.group(2)
            if not has_leading_doc(lines, i, "//"):
                missing.append((i + 1, "struct", name))
    return missing


def audit_c(path: Path) -> list[tuple[int, str, str]]:
    text = path.read_text(encoding="utf-8", errors="replace")
    lines = text.splitlines()
    missing = []
    for i, line in enumerate(lines):
        # Match function definitions: type name(...), but not calls or declarations without body.
        if C_FN_RE.match(line) and "{" in line or (i + 1 < len(lines) and lines[i + 1].strip() == "{"):
            # Extract the function name
            name = re.search(r"(\w+)\s*\(", line)
            if name:
                if not has_leading_doc(lines, i, "//"):
                    missing.append((i + 1, "fn", name.group(1)))
    return missing


def main() -> int:
    files = sorted(REPO.rglob("*"))
    out = []
    total_missing = 0
    for p in files:
        if p.is_symlink() or p.is_dir() or not p.is_file():
            continue
        if p.suffix == ".skink":
            missing = audit_skink(p)
        elif p.suffix == ".c":
            missing = audit_c(p)
        else:
            continue
        if missing:
            rel = p.relative_to(REPO)
            out.append(f"\n{rel} ({len(missing)} missing)")
            for line, kind, name in missing:
                out.append(f"  line {line:5d}: {kind} {name}")
            total_missing += len(missing)
    if total_missing:
        out.insert(0, f"Missing docstrings: {total_missing} across {sum(1 for _ in out if 'missing)' in _)} files")
    else:
        out.append("No missing docstrings found.")
    print("\n".join(out))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
