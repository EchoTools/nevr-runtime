"""The README's Building section names the prerequisites a first Linux/WSL build needs (#2039).

A first-time WSL build on 2026-11-03 stopped five times on prerequisites the section did not name: a CRLF checkout
from a Windows drive, VCPKG_ROOT, buf, submodules and config/public-defaults.env. Each check below is one of those.
"""

from __future__ import annotations

import pathlib

REPO = pathlib.Path(__file__).resolve().parents[2]
PREREQUISITE_MARKERS = (
    "VCPKG_ROOT",
    "buf",
    "git clone --recurse-submodules",
    "config/public-defaults.env",
    "core.autocrlf",
)


def building_section() -> str:
    """The text of README.md from its `## Building` header up to the next `## ` header."""
    lines = (REPO / "README.md").read_text(encoding="utf-8").splitlines()
    start = lines.index("## Building")
    end = next((i for i in range(start + 1, len(lines)) if lines[i].startswith("## ")), len(lines))
    return "\n".join(lines[start:end])


def test_building_section_names_each_prerequisite() -> None:
    section = building_section()
    missing = [marker for marker in PREREQUISITE_MARKERS if marker not in section]
    assert not missing, f"README Building section does not name: {missing}"
