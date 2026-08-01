#!/usr/bin/env python3
"""Verify that all cross-references between docs/ markdown files resolve.

Scans every .md file under docs/, extracts markdown links that point to other
docs/ files (relative paths containing no scheme and no absolute prefix), and
checks that each target exists on disk.  Exits non-zero if any link is broken.
"""

import os
import re
import sys
from pathlib import Path

DOCS_DIR = Path(__file__).resolve().parent.parent / "docs"

# Match [text](url) — capture the URL portion.
LINK_RE = re.compile(r"\[[^\]]*\]\(([^)]+)\)")


def extract_doc_links(file_path: Path) -> list[tuple[str, int]]:
    """Return (target, line_number) for every docs-internal link in file."""
    links: list[tuple[str, int]] = []
    try:
        text = file_path.read_text(encoding="utf-8")
    except OSError as exc:
        print(f"ERROR: cannot read {file_path}: {exc}", file=sys.stderr)
        return links

    for lineno, line in enumerate(text.splitlines(), start=1):
        for m in LINK_RE.finditer(line):
            target = m.group(1)
            # Skip external URLs and absolute paths.
            if "://" in target or target.startswith("/") or target.startswith("~"):
                continue
            # Skip anchor-only links within the same file.
            if target.startswith("#"):
                continue
            links.append((target, lineno))
    return links


def resolve_target(file_dir: Path, target: str) -> Path | None:
    """Resolve a relative link target against the file's directory."""
    # Strip anchor suffix.
    if "#" in target:
        target = target.split("#")[0]
    if not target:
        return None
    candidate = (file_dir / target).resolve()
    if candidate.exists():
        return candidate
    # Also try as a directory with README.md inside.
    if candidate.is_dir() and (candidate / "README.md").exists():
        return candidate / "README.md"
    return None


def main() -> int:
    errors = 0
    md_files = sorted(DOCS_DIR.rglob("*.md"))

    if not md_files:
        print("No .md files found under docs/", file=sys.stderr)
        return 1

    for md_file in md_files:
        links = extract_doc_links(md_file)
        for target, lineno in links:
            resolved = resolve_target(md_file.parent, target)
            if resolved is None:
                rel = md_file.relative_to(DOCS_DIR.parent)
                print(f"BROKEN: {rel}:{lineno} -> {target}", file=sys.stderr)
                errors += 1

    if errors:
        print(f"\n{errors} broken doc link(s) found.", file=sys.stderr)
        return 1

    print(f"OK: {len(md_files)} doc file(s), 0 broken links.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
