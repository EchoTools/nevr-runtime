"""Every tracked file that starts with a shebang is recorded executable.

The worktrees run with core.fileMode=false, so an exec bit set on disk is never recorded; a recipe
that runs such a file directly from a clean checkout then fails with exit 126 (Permission denied).
`git update-index --chmod=+x <path>` records it.
"""
from __future__ import annotations

import pathlib
import subprocess
import unittest

REPO = pathlib.Path(__file__).resolve().parents[2]


class ExecutableScriptsTest(unittest.TestCase):
    def test_every_tracked_shebang_file_is_mode_100755(self):
        listing = subprocess.run(["git", "-C", str(REPO), "ls-files", "-s", "-z"], check=True,
                                 capture_output=True, text=True).stdout
        entries = [e.split("\t", 1) for e in listing.split("\0") if e]
        self.assertGreater(len(entries), 100, "git ls-files returned almost nothing: the sensor is blind")
        recorded_plain = []
        for meta, path in entries:
            if meta.split()[0] != "100644":
                continue
            try:
                with open(REPO / path, "rb") as f:
                    starts_with_shebang = f.read(2) == b"#!"
            except OSError:
                continue
            if starts_with_shebang:
                recorded_plain.append(path)
        self.assertEqual(recorded_plain, [], "tracked scripts with a shebang recorded as 100644; run "
                         "`git update-index --chmod=+x <path>`")


if __name__ == "__main__":
    unittest.main()
