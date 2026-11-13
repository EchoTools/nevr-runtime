#!/usr/bin/env python3
"""Regression tests for the runtime patch-source inventory verifier (#1350).

The verifier holds no list of its own: every ``patch/*.cpp`` on disk must be in ``PATCHES_SOURCES`` once, and every
``patch/*.cpp`` entry must exist. A PR that adds a patch edits CMake and nothing else.
"""

import pathlib
import subprocess
import sys
import tempfile
import unittest


REPO = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "tools"))
import work_root  # noqa: E402
VERIFIER = REPO / "tools" / "verify_patch_source_inventory.py"

BASE = ("patch/a_patch.cpp", "patch/b_patch.cpp")


class VerifyPatchSourceInventoryTest(unittest.TestCase):
    def make_fixture(
        self,
        root: pathlib.Path,
        listed: tuple[str, ...],
        on_disk: tuple[str, ...] | None = None,
    ) -> pathlib.Path:
        """CMake lists `listed`; the files on disk are `on_disk` (default: the listed ones)."""
        runtime = root / "src" / "runtime"
        runtime.mkdir(parents=True)
        for source in set(listed if on_disk is None else on_disk):
            path = runtime / source
            path.parent.mkdir(parents=True, exist_ok=True)
            path.touch()

        cmake = runtime / "CMakeLists.txt"
        entries = "\n".join(f'    "{source}"' for source in listed)
        cmake.write_text(f"set(PATCHES_SOURCES\n{entries}\n)\n", encoding="utf-8")
        return cmake

    def run_verifier(self, cmake: pathlib.Path) -> subprocess.CompletedProcess[str]:
        return subprocess.run(
            [sys.executable, VERIFIER, "--cmake", cmake],
            capture_output=True,
            check=False,
            text=True,
        )

    def check(self, listed, on_disk=None) -> subprocess.CompletedProcess[str]:
        with tempfile.TemporaryDirectory(
            prefix="patch-source-inventory-", dir=work_root.test_scratch()
        ) as temp_dir:
            return self.run_verifier(self.make_fixture(pathlib.Path(temp_dir), listed, on_disk))

    def test_the_real_runtime_cmake_list_matches_the_tree(self):
        # The check `just verify` runs, also in `pytest tools/tests`, which every PR runs (#884 added two patch
        # sources and the pin stayed at 16 until the release build failed on it).
        result = subprocess.run([sys.executable, VERIFIER], capture_output=True, check=False, text=True)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_accepts_a_list_that_matches_the_disk(self):
        result = self.check(BASE)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("patch-source-inventory: OK count=2", result.stdout)

    def test_a_source_added_in_cmake_and_on_disk_passes_with_no_other_edit(self):
        # Two PRs that each add one source edit CMake and add a file. No second list or count to collide (#2207, #1350).
        result = self.check(BASE + ("patch/added_by_one_pr.cpp",))
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_a_patch_file_planted_on_disk_but_not_in_cmake_fails(self):
        # The planted case of #1350: a patch source that CMake does not compile.
        result = self.check(BASE, on_disk=BASE + ("patch/planted.cpp",))
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn("patch source on disk is not in PATCHES_SOURCES: patch/planted.cpp", result.stderr)

    def test_a_cmake_entry_with_no_file_fails(self):
        result = self.check(BASE, on_disk=BASE[:1])
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn("PATCHES_SOURCES patch source is missing from disk: patch/b_patch.cpp", result.stderr)

    def test_a_duplicate_entry_fails(self):
        result = self.check(BASE + ("patch/a_patch.cpp",))
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn("duplicate PATCHES_SOURCES patch entry: patch/a_patch.cpp", result.stderr)

    def test_rejects_missing_authoritative_list(self):
        with tempfile.TemporaryDirectory(
            prefix="patch-source-inventory-", dir=work_root.test_scratch()
        ) as temp_dir:
            cmake = pathlib.Path(temp_dir) / "src" / "runtime" / "CMakeLists.txt"
            cmake.parent.mkdir(parents=True)
            cmake.write_text("set(OTHER_SOURCES)\n", encoding="utf-8")
            result = self.run_verifier(cmake)

        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn("PATCHES_SOURCES list not found", result.stderr)

    def test_rejects_unreadable_authoritative_file(self):
        with tempfile.TemporaryDirectory(
            prefix="patch-source-inventory-", dir=work_root.test_scratch()
        ) as temp_dir:
            result = self.run_verifier(pathlib.Path(temp_dir) / "missing-CMakeLists.txt")

        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn("cannot read", result.stderr)


if __name__ == "__main__":
    unittest.main()
