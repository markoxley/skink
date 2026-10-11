#!/usr/bin/env python3
"""Add the canonical Skink file header to all source files."""

import os
import re
import sys
from pathlib import Path

REPO = Path("/home/mark/Documents/GitHub/skink")

PROJECT_LINES = [
    "A self-hosted, statically-typed programming language",
    "targeting LLVM IR, designed for systems programming with",
    "modern ergonomics and a Go-inspired syntax.",
]

APACHE_LINES = [
    'Licensed under the Apache License, Version 2.0 (the "License");',
    "you may not use this file except in compliance with the License.",
    "You may obtain a copy of the License at",
    "",
    "    http://www.apache.org/licenses/LICENSE-2.0",
    "",
    "Unless required by applicable law or agreed to in writing, software",
    'distributed under the License is distributed on an "AS IS" BASIS,',
    "WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.",
    "See the License for the specific language governing permissions and",
    "limitations under the License.",
]


def comment_char_for(path: Path) -> str:
    ext = path.suffix
    if ext in (".skink", ".c"):
        return "//"
    if ext == ".ll":
        return ";"
    if ext == ".sh":
        return "#"
    raise ValueError(f"unsupported extension: {ext}")


def build_header(relpath: str, purpose: str, ch: str) -> str:
    lines = [
        f"{ch} Copyright 2026 Mark Oxley. All rights reserved.",
        f"{ch}",
        f"{ch} Project: Skink — {PROJECT_LINES[0]}",
    ]
    for pl in PROJECT_LINES[1:]:
        lines.append(f"{ch}          {pl}")
    lines.append(f"{ch}")
    lines.append(f"{ch} File:    {relpath}")
    lines.append(f"{ch} Purpose: {purpose}")
    lines.append(f"{ch} Author:  Mark Oxley")
    lines.append(f"{ch} Created: 2026")
    lines.append(f"{ch}")
    for al in APACHE_LINES:
        if al == "":
            lines.append(f"{ch}")
        else:
            lines.append(f"{ch} {al}")
    return "\n".join(lines) + "\n"


def extract_purpose(lead_lines: list[str]) -> str | None:
    """Try to extract a one-line purpose from an existing leading comment block."""
    text = " ".join(lead_lines)
    text = re.sub(r"\s+", " ", text).strip()
    if not text:
        return None
    # Use the first sentence if it is reasonably short, else the first clause/line.
    m = re.match(r"(.{10,120}?[.!?])(\s|$)", text)
    if m:
        return m.group(1).strip()
    if len(text) > 120:
        return text[:120].strip() + "..."
    return text


def derive_purpose_from_name(relpath: str) -> str:
    """Fallback purpose derived from the file name."""
    name = Path(relpath).name
    base = Path(name).stem.replace("_", " ")
    if relpath.endswith(".skink"):
        return f"Skink module for {base}."
    if relpath.endswith(".c"):
        return f"C runtime support for {base}."
    if relpath.endswith(".ll"):
        return f"LLVM IR bridge/runtime for {base}."
    if relpath.endswith(".sh"):
        return f"Shell script for {base}."
    return f"Source file for {base}."


def has_canonical_header(lines: list[str], ch: str) -> bool:
    """Check whether the leading comment block already matches the canonical header."""
    joined = "\n".join(lines)
    return (
        "Copyright 2026 Mark Oxley" in joined
        and "File:" in joined
        and "Purpose:" in joined
        and "Author:" in joined
        and "Created: 2026" in joined
        and "Licensed under the Apache License" in joined
    )


def collect_leading_block(lines: list[str], ch: str) -> tuple[list[str], list[str]]:
    """Return (leading_comment_lines, remaining_lines)."""
    i = 0
    leading = []
    while i < len(lines):
        line = lines[i]
        stripped = line.strip()
        if stripped == "":
            leading.append(line)
            i += 1
            continue
        if stripped.startswith(ch) or (ch == "#" and stripped.startswith("#!")):
            leading.append(line)
            i += 1
            continue
        break
    return leading, lines[i:]


def is_minimal_license_block(block: list[str], ch: str) -> bool:
    """Detect a bare copyright+license block without the full project/file fields."""
    joined = "\n".join(block)
    has_copyright = "Copyright" in joined
    has_license = "Licensed under" in joined or "Apache License" in joined
    has_full = "File:" in joined and "Purpose:" in joined
    return has_copyright and has_license and not has_full


def process_file(path: Path) -> tuple[bool, str]:
    rel = str(path.relative_to(REPO))
    ch = comment_char_for(path)
    raw = path.read_text(encoding="utf-8", errors="replace")
    original_lines = raw.splitlines(keepends=True)

    shebang = ""
    if path.suffix == ".sh" and original_lines and original_lines[0].startswith("#!"):
        shebang = original_lines[0]
        rest = original_lines[1:]
    else:
        rest = original_lines

    # Remove leading blank lines for easier block detection.
    while rest and rest[0].strip() == "":
        rest.pop(0)

    leading, remainder = collect_leading_block(rest, ch)

    if has_canonical_header(leading, ch):
        return False, "already has canonical header"

    # Decide whether the leading block is a minimal license (replace) or a
    # description (keep after the new header).
    if leading and is_minimal_license_block(leading, ch):
        purpose = derive_purpose_from_name(rel)
        existing_desc = []
    else:
        # Strip the comment markers to use as description text.
        desc_lines = []
        for line in leading:
            stripped = line.strip()
            if stripped.startswith(ch):
                stripped = stripped[len(ch):].strip()
            if stripped:
                desc_lines.append(stripped)
        purpose = extract_purpose(desc_lines) or derive_purpose_from_name(rel)
        existing_desc = leading if leading and desc_lines else []

    header = build_header(rel, purpose, ch)

    new_lines = []
    if shebang:
        new_lines.append(shebang)
        # Ensure a blank line after shebang before header.
        if not new_lines[-1].endswith("\n"):
            new_lines[-1] += "\n"
    new_lines.append(header)
    if existing_desc:
        new_lines.append("\n")
        new_lines.extend(existing_desc)
        # Ensure a blank line before the remainder.
        if existing_desc[-1].strip() != "":
            new_lines.append("\n")
    else:
        # One blank line after the license.
        new_lines.append("\n")
    new_lines.extend(remainder)

    path.write_text("".join(new_lines), encoding="utf-8")
    return True, "header added"


def find_source_files() -> list[Path]:
    exts = {".skink", ".c", ".ll", ".sh"}
    files = []
    for root, _dirs, names in os.walk(REPO):
        # Skip git and build artifacts.
        if "/.git/" in root or root.endswith("/.git"):
            continue
        for name in names:
            p = Path(root) / name
            if p.suffix in exts and not p.is_symlink():
                # Exclude compiled binaries and build artifacts by name.
                if name in {"skink", "skink_new"}:
                    continue
                files.append(p)
    return sorted(files)


def main() -> int:
    files = find_source_files()
    changed = 0
    skipped = 0
    errors = 0
    for p in files:
        try:
            did_change, msg = process_file(p)
            if did_change:
                changed += 1
                print(f"+ {p.relative_to(REPO)}: {msg}")
            else:
                skipped += 1
                print(f"  {p.relative_to(REPO)}: {msg}")
        except Exception as e:
            errors += 1
            print(f"! {p.relative_to(REPO)}: {e}", file=sys.stderr)
    print(f"\nChanged: {changed}, Skipped: {skipped}, Errors: {errors}")
    return 0 if errors == 0 else 1


if __name__ == "__main__":
    raise SystemExit(main())
