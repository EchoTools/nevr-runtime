"""Find a recipe in the justfile by its name, whatever its header form (#1910).

A recipe is a header at column 0, `name`, then optional parameters, then `:` and optional dependencies, followed by
its body: the lines indented under it, and the blank lines between them. The body ends at the next line at column 0
(another header, a variable, a column-0 comment or the end of the file). Header forms in the justfile include:

    verify:                                             plain
    verify *args: (_heavy-guard "verify" args)          variadic parameter, a dependency in parentheses
    _quest-apk store_apk *args: (_heavy-guard ...) dep  parameter and several dependencies
    build-checkout rev="origin/main":                   a parameter with a default

A `[positional-arguments]` attribute line above a header is not part of the header and not part of the body.
Variable assignments (`preset := ...`) are not recipes.
"""

from __future__ import annotations

import pathlib
import re

REPO = pathlib.Path(__file__).resolve().parents[2]


def read_justfile() -> str:
    """The repository's justfile, as text."""
    return (REPO / "justfile").read_text(encoding="utf-8")


def _header_match(text: str, name: str) -> re.Match | None:
    # The name, an optional run of parameters (no colon in them), then a colon that is not an assignment (`:=`).
    pattern = rf"^{re.escape(name)}(?:[ \t][^\n:]*)?:(?![=:])([^\n]*)\n"
    return re.search(pattern, text, re.M)


def header(text: str, name: str) -> str | None:
    """The header line of recipe `name` (without its newline), or None when there is no such recipe."""
    match = _header_match(text, name)
    return None if match is None else match.group(0).rstrip("\n")


def body(text: str, name: str) -> str | None:
    """The body of recipe `name`: its indented and blank lines after the header, trailing blank lines removed.

    Returns "" for a recipe with no body, and None when there is no such recipe. Every line keeps its indentation and
    the text ends with a newline when there is any body.
    """
    match = _header_match(text, name)
    if match is None:
        return None
    lines: list[str] = []
    for line in text[match.end():].splitlines(keepends=True):
        if line.strip() and not line[0] in " \t":
            break
        lines.append(line)
    while lines and not lines[-1].strip():
        lines.pop()
    return "".join(lines)


def recipe(text: str, name: str) -> str | None:
    """The header line and the body of recipe `name`, as the justfile writes them, or None when there is no such recipe."""
    head = header(text, name)
    if head is None:
        return None
    return head + "\n" + (body(text, name) or "")


def names(text: str) -> list[str]:
    """The names of the recipes in the justfile, in file order (variables and attributes excluded)."""
    found = []
    for match in re.finditer(r"^([A-Za-z_][\w-]*)(?:[ \t][^\n:]*)?:(?![=:])", text, re.M):
        found.append(match.group(1))
    return found
