#!/usr/bin/env python3
"""AGENTS.md imports the standards it makes part of its contract (#1277).

A standard AGENTS.md only names is read when an agent remembers to read it. An
`@<path>` line is expanded into the instructions of a session that loads
AGENTS.md, so the standard is in context from the first turn. This check keeps
each import where it works: on a line of its own, unindented, outside a code
fence, before the first `##` section, naming a file that exists, with the
sentence that makes it binding for a reader that does not expand imports.

Exit 0 = every import is in place. Exit 1 = one is missing or misplaced.
"""
import argparse
import pathlib
import sys

REPO = pathlib.Path(__file__).resolve().parent.parent

# (import line, the sentence before it that makes the imported file binding)
IMPORTS = [
    ("@docs/standards/logging.md", "The logging standard below is part of this contract"),
]


def preamble(text: str) -> list[str]:
    """Lines of AGENTS.md before the first `##` section, with fenced code blocks removed."""
    lines = []
    fenced = False
    for line in text.splitlines():
        if line.lstrip().startswith("```"):
            fenced = not fenced
            continue
        if fenced:
            continue
        if line.startswith("## "):
            break
        lines.append(line)
    return lines


def check(root: pathlib.Path) -> list[str]:
    agents = root / "AGENTS.md"
    try:
        text = agents.read_text(encoding="utf-8")
    except OSError as exc:
        return [f"cannot read {agents}: {exc}"]
    head = preamble(text)
    errors = []
    for line, sentence in IMPORTS:
        target = line[1:]
        if line not in head:
            errors.append(f"AGENTS.md has no `{line}` line (unindented, outside a code fence) before its first `##` section")
        if not any(sentence in s for s in head):
            errors.append(f"AGENTS.md has no \"{sentence}\" before its first `##` section")
        if not (root / target).is_file():
            errors.append(f"`{line}` names {target}, which does not exist")
    return errors


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--root", type=pathlib.Path, default=REPO, help="repository root (default: this checkout)")
    args = parser.parse_args()
    errors = check(args.root)
    for error in errors:
        print(f"agents-imports: FAIL — {error}", file=sys.stderr)
    if errors:
        return 1
    print("agents-imports OK: " + ", ".join(line for line, _ in IMPORTS))
    return 0


if __name__ == "__main__":
    sys.exit(main())
