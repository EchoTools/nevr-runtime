#!/usr/bin/env python3
"""Every tracked script with a shebang under tools/ or a tests directory is recorded executable,
except the frozen list of scripts that were already recorded 100644 (#230).

The worktrees run with core.fileMode=false, so an exec bit that exists only on disk never reaches
git, and a recipe that runs the script directly fails from a clean checkout with exit 126.
"""

from __future__ import annotations

import pathlib
import subprocess
import sys
import unittest

REPO = pathlib.Path(__file__).resolve().parents[2]

# Scripts that already had a shebang and mode 100644 on origin/main when this sensor was added
# (#230 tracks fixing them). The list only shrinks: a path fixed to 100755 must be removed here,
# and a new non-executable shebang script is not allowed in.
KNOWN_NON_EXECUTABLE = frozenset(
    {
        "tools/build_distribution.py",
        "tools/echomod/clone_combat.py",
        "tools/echomod/clone_frisbee.py",
        "tools/echomod/generate_resources.py",
        "tools/echomod/rad_archive_tool.py",
        "tools/echomod/setuparchive.py",
        "tools/gen_symbol_corpus.py",
        "tools/generate-symcache.sh",
        "tools/scenario/control.py",
        "tools/scenario/run_all.py",
        "tools/scenario/run_scenario.py",
        "tools/tests/test_quest_shared_redirect_sources.py",
        "tools/tests/test_sample_config_sensor.py",
        "tools/tests/test_score_log_markers.py",
        "tools/tests/test_verify_doc_paths.py",
        "tools/tests/test_verify_hook_invariants.py",
        "tools/tests/test_verify_patch_source_inventory.py",
        "tools/tests/test_winvm_checks.py",
        "tools/verify_doc_paths.py",
        "tools/verify_hook_invariants.py",
        "tools/verify_log_rules.py",
        "tools/verify_mode_patch_ground_truth.py",
        "tools/verify_patch_source_inventory.py",
        "tools/verify_scenario_control_absent.py",
        "tools/winvm/dump_stacks.py",
        "tools/winvm/systest.py",
    }
)


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
    def test_no_new_non_executable_shebang_script(self) -> None:
        found = set(non_executable_scripts(tracked_entries()))
        self.assertEqual(sorted(found - KNOWN_NON_EXECUTABLE), [])

    def test_the_known_list_only_shrinks(self) -> None:
        found = set(non_executable_scripts(tracked_entries()))
        self.assertEqual(
            sorted(KNOWN_NON_EXECUTABLE - found),
            [],
            "these are executable (or gone) now: remove them from KNOWN_NON_EXECUTABLE",
        )

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
