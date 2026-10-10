#!/usr/bin/env python3
"""tools/quest_link_objects.py against a synthetic build.ninja.

The object list the static-initializer check scans must be what the linker was given: the shared
object's own objects, plus the members of every in-tree static archive on its link line, and nothing
a removed target left behind in the build tree.
"""

from __future__ import annotations

import pathlib
import subprocess
import sys
import tempfile
import unittest

REPO = pathlib.Path(__file__).resolve().parents[2]
SCRIPT = REPO / "tools" / "quest_link_objects.py"

NINJA = """\
rule CXX_SHARED_LIBRARY_LINKER__so
rule CXX_STATIC_LIBRARY_LINKER__lib
build sentinel/CMakeFiles/so.dir/entry.cpp.o: CXX_COMPILER__so ../entry.cpp
build libredirect.a: CXX_STATIC_LIBRARY_LINKER__lib CMakeFiles/redirect.dir/a.cpp.o CMakeFiles/redirect.dir/b.cpp.o
build libtop.a: CXX_STATIC_LIBRARY_LINKER__lib CMakeFiles/top.dir/t.cpp.o | libredirect.a
build libgot.a: CXX_STATIC_LIBRARY_LINKER__lib CMakeFiles/got.dir/g.cpp.o
build sentinel/libso.so: CXX_SHARED_LIBRARY_LINKER__so sentinel/CMakeFiles/so.dir/entry.cpp.o | libtop.a vendor/libcurl.a libgot.a || libunused.a
build libunused.a: CXX_STATIC_LIBRARY_LINKER__lib CMakeFiles/unused.dir/u.cpp.o
"""

OBJECTS = [
    "sentinel/CMakeFiles/so.dir/entry.cpp.o",
    "CMakeFiles/redirect.dir/a.cpp.o",
    "CMakeFiles/redirect.dir/b.cpp.o",
    "CMakeFiles/top.dir/t.cpp.o",
    "CMakeFiles/got.dir/g.cpp.o",
    "CMakeFiles/unused.dir/u.cpp.o",
    "CMakeFiles/stale.dir/stale.cpp.o",  # in the tree, on no link line
]


class QuestLinkObjectsTest(unittest.TestCase):
    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self._tmp.cleanup)
        self.build = pathlib.Path(self._tmp.name)
        (self.build / "build.ninja").write_text(NINJA, encoding="utf-8")
        for name in OBJECTS:
            path = self.build / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(b"\x7fELF")

    def run_script(self, output: str = "sentinel/libso.so") -> subprocess.CompletedProcess:
        return subprocess.run(
            [sys.executable, "-I", str(SCRIPT), str(self.build), output],
            capture_output=True, text=True, check=False)

    def listed(self, result: subprocess.CompletedProcess) -> set[str]:
        return {str(pathlib.Path(line).relative_to(self.build)) for line in result.stdout.splitlines()}

    def test_lists_own_objects_and_archive_members_not_stale_or_order_only(self) -> None:
        result = self.run_script()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(self.listed(result), {
            "sentinel/CMakeFiles/so.dir/entry.cpp.o",
            "CMakeFiles/redirect.dir/a.cpp.o",  # through libtop.a -> libredirect.a
            "CMakeFiles/redirect.dir/b.cpp.o",
            "CMakeFiles/top.dir/t.cpp.o",
            "CMakeFiles/got.dir/g.cpp.o",
        })

    def test_names_archives_it_cannot_open_on_stderr(self) -> None:
        result = self.run_script()
        self.assertIn("vendor/libcurl.a", result.stderr)

    def test_missing_link_statement_fails_closed(self) -> None:
        result = self.run_script("sentinel/libnothing.so")
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(result.stdout, "")
        self.assertIn("no build statement", result.stderr)

    def test_missing_object_fails_closed(self) -> None:
        (self.build / "CMakeFiles/got.dir/g.cpp.o").unlink()
        result = self.run_script()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("g.cpp.o", result.stderr)

    def test_link_with_no_objects_fails_closed(self) -> None:
        (self.build / "build.ninja").write_text(
            "build sentinel/libso.so: CXX_SHARED_LIBRARY_LINKER__so | vendor/libcurl.a\n", encoding="utf-8")
        result = self.run_script()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("links no object file", result.stderr)


if __name__ == "__main__":
    unittest.main()
