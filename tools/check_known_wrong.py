#!/usr/bin/env python3
"""A state the design calls wrong is marked `KNOWN-WRONG #<issue>`, never written as the spec (#1302).

Four checks over the tracked tree, each a list of `path:line: message` problems:

- a `tools/tests` `assertNotIn` whose message, or the comment block right above it, defers a fix
  ("ADR 0012 step 6", "not yet", "until"): the test pins the fix's absence;
- a markdown paragraph, list item or table row that defers a build ("not yet implemented", "until ... lands")
  and names no issue;
- a smoke-checklist row whose pass condition is "Expect none" and that carries no marker;
- a `KNOWN-WRONG` with no `#<number>` right after it.

`--online` also asks GitHub (`gh issue view`) that every issue a marker names is open. Exit 0 when nothing is
found, 1 when something is, 2 when the tree cannot be read.
"""
from __future__ import annotations

import argparse
import ast
import os
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

MARKER = re.compile(r"KNOWN-WRONG(?! #\d)")
MARKED = re.compile(r"KNOWN-WRONG #(\d+)")
ISSUE = re.compile(r"(?<![\w/&])#\d+\b")
TEST_DEFERS = re.compile(
    r"\bADR \d+,? step \d+\b|\bstep \d+\b|\bnot yet\b|\buntil\b|\bfor now\b|\bdeferred\b|\ba later (?:change|step|PR)\b",
    re.I)
DOC_DEFERS = re.compile(
    r"\bnot yet (?:implemented|wired|built|moved|supported|decided|shipped|done|in the root)\b"
    r"|\buntil [^.;:]{0,60}?\b(?:lands|decides|ships|is wired)\b",
    re.I)
EXPECT_NONE = re.compile(r"\bExpect none\b", re.I)
CODE_SPAN = re.compile(r"`[^`\n]*`")  # markdown quotes a phrase in backticks to name it, not to say it
SKIP_DIRS = ("docs/audits/", "extern/")  # the audit record is dated by design (AGENTS.md); extern is not ours
TEXT_SUFFIXES = (".md", ".py", ".cpp", ".h", ".hpp", ".c", ".sh", ".ps1", ".go", ".rs", ".yml", ".yaml", ".txt",
                 ".cmake", ".json")


def tracked(root: Path) -> list[str]:
    out = subprocess.run(["git", "-C", str(root), "ls-files", "-z"], capture_output=True, check=True).stdout
    return [p for p in out.decode("utf-8").split("\0") if p and not p.startswith(SKIP_DIRS)]


def message_text(node: ast.AST | None) -> str:
    if isinstance(node, ast.Constant) and isinstance(node.value, str):
        return node.value
    if isinstance(node, ast.JoinedStr):
        return "".join(v.value for v in node.values if isinstance(v, ast.Constant) and isinstance(v.value, str))
    return ""


def deferring_assertions(root: Path, files: list[str]) -> list[str]:
    problems = []
    for rel in files:
        if not (rel.startswith("tools/tests/") and Path(rel).name.startswith("test_") and rel.endswith(".py")):
            continue
        source = (root / rel).read_text(encoding="utf-8")
        lines = source.splitlines()
        for node in ast.walk(ast.parse(source, rel)):
            if not (isinstance(node, ast.Call) and isinstance(node.func, ast.Attribute)
                    and node.func.attr == "assertNotIn"):
                continue
            msg = node.args[2] if len(node.args) > 2 else next((k.value for k in node.keywords if k.arg == "msg"), None)
            comments, i = [], node.lineno - 2
            while i >= 0 and lines[i].strip().startswith("#"):
                comments.append(lines[i].strip())
                i -= 1
            found = TEST_DEFERS.search(" ".join([message_text(msg), *comments]))
            if found:
                problems.append(f"{rel}:{node.lineno}: assertNotIn pins the absence of a deferred fix "
                                f"(\"{found.group(0)}\"); mark the site KNOWN-WRONG #<issue> instead")
    return problems


def units(text: str) -> list[tuple[int, str]]:
    """Paragraphs, list items, table rows and headings of a markdown file, outside code fences, with their line."""
    found: list[tuple[int, str]] = []
    current: list[str] = []
    start = 0
    fence = False

    def flush() -> None:
        if current:
            found.append((start, " ".join(current)))
        current.clear()

    for number, line in enumerate(text.splitlines(), 1):
        stripped = line.strip()
        if stripped.startswith("```"):
            flush()
            fence = not fence
            continue
        if fence or not stripped:
            flush()
            continue
        heading_or_row = stripped.startswith("|") or re.match(r"#{1,6}\s", stripped)
        if heading_or_row or re.match(r"([-*+]|\d+\.)\s", stripped):
            flush()
            start = number
            current.append(stripped)
            if heading_or_row:
                flush()
            continue
        if not current:
            start = number
        current.append(stripped)
    flush()
    return found


def untracked_deferrals(root: Path, files: list[str]) -> list[str]:
    problems = []
    for rel in files:
        if not rel.endswith(".md"):
            continue
        for line, unit in units((root / rel).read_text(encoding="utf-8")):
            found = DOC_DEFERS.search(CODE_SPAN.sub("", unit))
            if found and not ISSUE.search(unit):
                problems.append(f"{rel}:{line}: \"{found.group(0)}\" defers a build and names no issue")
    return problems


def expect_none_rows(root: Path, files: list[str]) -> list[str]:
    problems = []
    for rel in files:
        if not (rel.startswith("docs/testing/") and rel.endswith(".md")):
            continue
        for number, line in enumerate((root / rel).read_text(encoding="utf-8").splitlines(), 1):
            if line.lstrip().startswith("|") and EXPECT_NONE.search(line) and not MARKED.search(line):
                problems.append(f"{rel}:{number}: a row passes on \"Expect none\"; mark it KNOWN-WRONG #<issue>")
    return problems


def unnumbered_markers(root: Path, files: list[str]) -> list[str]:
    problems = []
    for rel in files:
        if not (rel.endswith(TEXT_SUFFIXES) or Path(rel).name in ("justfile", "Makefile")):
            continue
        if rel == "tools/check_known_wrong.py" or rel == "tools/tests/test_known_wrong.py":
            continue  # the checker and its test spell the marker as data
        try:
            text = (root / rel).read_text(encoding="utf-8")
        except UnicodeDecodeError:
            continue
        pieces = ([(n, CODE_SPAN.sub("", u)) for n, u in units(text)] if rel.endswith(".md")
                  else enumerate(text.splitlines(), 1))
        for number, line in pieces:
            if MARKER.search(line):
                problems.append(f"{rel}:{number}: KNOWN-WRONG names no issue (write KNOWN-WRONG #<number>)")
    return problems


def marked_issues(root: Path, files: list[str]) -> set[int]:
    issues: set[int] = set()
    for rel in files:
        if rel in ("tools/check_known_wrong.py", "tools/tests/test_known_wrong.py"):
            continue
        try:
            text = (root / rel).read_text(encoding="utf-8")
        except (UnicodeDecodeError, IsADirectoryError, FileNotFoundError):
            continue
        issues.update(int(n) for n in MARKED.findall(text))
    return issues


def closed_issues(issues: set[int], gh: str) -> list[str]:
    problems = []
    for number in sorted(issues):
        result = subprocess.run([gh, "issue", "view", str(number), "--json", "state", "-q", ".state"],
                                capture_output=True, text=True)
        state = result.stdout.strip()
        if result.returncode != 0:
            problems.append(f"#{number}: gh could not read the issue: {result.stderr.strip()}")
        elif state != "OPEN":
            problems.append(f"#{number}: a KNOWN-WRONG marker names an issue that is {state}; "
                            "the fix landed (remove the marker) or the issue closed without it (reopen or re-file)")
    return problems


def check(root: Path, online: bool = False, gh: str = "gh") -> list[str]:
    files = tracked(root)
    problems = (deferring_assertions(root, files) + untracked_deferrals(root, files)
                + expect_none_rows(root, files) + unnumbered_markers(root, files))
    if online:
        problems += closed_issues(marked_issues(root, files), gh)
    return problems


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--root", type=Path, default=ROOT)
    parser.add_argument("--online", action="store_true", help="also require every marked issue to be open (gh)")
    args = parser.parse_args(argv)
    try:
        problems = check(args.root, args.online, os.environ.get("GH", "gh"))
    except (OSError, subprocess.CalledProcessError, SyntaxError) as error:
        print(f"known-wrong: cannot read the tree at {args.root}: {error}", file=sys.stderr)
        return 2
    for problem in problems:
        print(f"known-wrong: FAIL — {problem}", file=sys.stderr)
    if problems:
        return 1
    print("known-wrong OK: no deferring assertNotIn, no untracked deferral, no unmarked \"Expect none\", "
          "every KNOWN-WRONG names an issue" + (" (all open)" if args.online else ""))
    return 0


if __name__ == "__main__":
    sys.exit(main())
