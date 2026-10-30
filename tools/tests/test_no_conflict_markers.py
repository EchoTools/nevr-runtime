"""No tracked C++ source, header or build file under src/, sdk/ or tools/ holds a merge conflict marker.

A squash merge of #1342 left a stray `=======` line and a corrupted section header in src/runtime/hook/addresses.h, which
no check noticed and which stopped every PC build with "version control conflict marker in file"."""
from __future__ import annotations

import pathlib
import re
import subprocess
import unittest

REPO = pathlib.Path(__file__).resolve().parents[2]
SOURCE = (".h", ".hpp", ".cpp", ".cc", ".c", ".def", ".rc", ".cmake", ".txt", ".py", ".sh")
# A marker starts a line: seven of `<`, `>` or `=` (the `<` and `>` ones carry a label after a space), and a
# `// ====...====` banner glued to a second banner on one line.
MARKER = re.compile(r"^(?:<<<<<<<(?: |$)|>>>>>>>(?: |$)|=======$)")
GLUED_BANNER = re.compile(r"=={20,}\s*//\s*={20,}")


def tracked_sources() -> list[pathlib.Path]:
    out = subprocess.run(["git", "ls-files", "-z", "--", "src", "sdk", "tools", "CMakeLists.txt"], cwd=REPO,
                         capture_output=True, check=True).stdout.decode().split("\0")
    return [REPO / name for name in out if name.endswith(SOURCE) and (REPO / name).is_file()]


def damage(text: str) -> list[tuple[int, str]]:
    return [(n, row) for n, row in enumerate(text.split("\n"), 1) if MARKER.match(row) or GLUED_BANNER.search(row)]


class ConflictMarkerTest(unittest.TestCase):
    def test_the_detector_finds_the_damage_that_reached_addresses_h(self):
        self.assertEqual([n for n, _ in damage("a\n=======\nb")], [2])
        self.assertEqual([n for n, _ in damage("<<<<<<< HEAD\nx\n>>>>>>> main")], [1, 3])
        self.assertEqual([n for n, _ in damage("// " + "=" * 69 + "// " + "=" * 76)], [1])
        self.assertEqual(damage("// " + "=" * 76 + "\nint a = b == c;\n"), [])

    def test_no_tracked_source_holds_a_conflict_marker(self):
        found = {str(p.relative_to(REPO)): damage(p.read_text(errors="replace")) for p in tracked_sources()}
        found = {name: rows for name, rows in found.items() if rows}
        self.assertEqual(found, {})


if __name__ == "__main__":
    unittest.main()
