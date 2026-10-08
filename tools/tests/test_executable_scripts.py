#!/usr/bin/env python3
"""Every tracked script with a shebang under tools/ or a tests directory is recorded executable.

The worktrees run with core.fileMode=false, so an exec bit that exists only on disk never reaches
git, and a recipe that runs the script directly fails from a clean checkout with exit 126.
"""

from __future__ import annotations

import pathlib
import subprocess
import sys
import unittest

REPO = pathlib.Path(__file__).resolve().parents[2]


def in_scope(path: str) -> bool:
    return path.startswith(("tools/", "tests/")) or "/tests/" in path


def non_executable_scripts(entries: list[tuple[str, str, bytes]]) -> list[str]:
    """`entries` are (mode, path, first line of the blob). Returns the offending paths."""
    return sorted(
        path
        for mode, path, first_line in entries
        if mode in ("100644", "100664") and in_scope(path) and first_line.startswith(b"#!")
    )


def tracked_entries() -> list[tuple[str, str, bytes]]:
    """What git records (the index), not what is on disk."""
    listing = subprocess.run(["git", "ls-files", "-s", "-z"], cwd=REPO, capture_output=True, check=True).stdout
    entries: list[tuple[str, str, bytes]] = []
    for record in listing.split(b"\0"):
        if not record:
            continue
        meta, path = record.split(b"\t", 1)
        mode, blob = meta.decode().split()[0], meta.decode().split()[1]
        text = path.decode()
        if mode == "120000" or not in_scope(text):
            continue
        content = subprocess.run(["git", "cat-file", "blob", blob], cwd=REPO, capture_output=True, check=True).stdout
        entries.append((mode, text, content.split(b"\n", 1)[0]))
    return entries


class ExecutableScriptsTest(unittest.TestCase):
    def test_every_tracked_shebang_script_is_mode_100755(self) -> None:
        self.assertEqual(non_executable_scripts(tracked_entries()), [])

    def test_the_scan_flags_only_non_executable_shebang_scripts_in_scope(self) -> None:
        entries = [
            ("100644", "tools/a.sh", b"#!/usr/bin/env bash"),
            ("100755", "tools/b.sh", b"#!/usr/bin/env bash"),
            ("100644", "tools/c.md", b"# heading"),
            ("100644", "src/quest/tests/d.py", b"#!/usr/bin/env python3"),
            ("100644", "src/runtime/e.sh", b"#!/usr/bin/env bash"),
            ("100644", "tests/f.py", b""),
        ]
        self.assertEqual(non_executable_scripts(entries), ["src/quest/tests/d.py", "tools/a.sh"])


if __name__ == "__main__":
    sys.exit(unittest.main())
