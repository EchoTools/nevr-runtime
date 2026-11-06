#!/usr/bin/env python3
"""Regression tests for the runtime patch-source inventory verifier."""

import contextlib
import io
import pathlib
import subprocess
import sys
import tempfile
import unittest
from unittest import mock


REPO = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "tools"))
import verify_patch_source_inventory as verifier  # noqa: E402
import work_root  # noqa: E402
VERIFIER = REPO / "tools" / "verify_patch_source_inventory.py"


def accepted_fixture_sources() -> tuple[str, ...]:
    return verifier.EXPECTED_PATCH_SOURCES


class VerifyPatchSourceInventoryTest(unittest.TestCase):
    def make_fixture(
        self,
        root: pathlib.Path,
        sources: tuple[str, ...],
        absent_files: tuple[str, ...] = (),
    ) -> pathlib.Path:
        runtime = root / "src" / "runtime"
        runtime.mkdir(parents=True)
        for source in set(sources) - set(absent_files):
            path = runtime / source
            path.parent.mkdir(parents=True, exist_ok=True)
            path.touch()

        cmake = runtime / "CMakeLists.txt"
        entries = "\n".join(f'    "{source}"' for source in sources)
        cmake.write_text(f"set(PATCHES_SOURCES\n{entries}\n)\n", encoding="utf-8")
        return cmake

    def run_verifier(self, cmake: pathlib.Path) -> subprocess.CompletedProcess[str]:
        return subprocess.run(
            [sys.executable, VERIFIER, "--cmake", cmake],
            capture_output=True,
            check=False,
            text=True,
        )

    def test_the_real_runtime_cmake_list_matches_the_pin(self):
        # The pin is what `just verify` checks; running it here puts it in `pytest tools/tests`, which every PR
        # runs (#884 added two patch sources and the pin stayed at 16 until the release build failed on it).
        result = subprocess.run([sys.executable, VERIFIER], capture_output=True, check=False, text=True)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_accepts_complete_authoritative_fixture(self):
        with tempfile.TemporaryDirectory(
            prefix="patch-source-inventory-", dir=work_root.test_scratch()
        ) as temp_dir:
            result = self.run_verifier(
                self.make_fixture(pathlib.Path(temp_dir), accepted_fixture_sources())
            )

        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn(f"patch-source-inventory: OK count={len(accepted_fixture_sources())}", result.stdout)

    def test_the_acceptance_fixture_takes_the_checkers_list(self):
        # A PR that adds a source edits the checker's list and PATCHES_SOURCES, and nothing else. The fixture the
        # acceptance tests build must follow the checker's list: a copy of the list here is a third place to edit.
        added = "patch/added_by_one_pr.cpp"
        with mock.patch.object(verifier, "EXPECTED_PATCH_SOURCES", verifier.EXPECTED_PATCH_SOURCES + (added,)):
            self.assertIn(added, accepted_fixture_sources())

    def test_a_source_added_to_both_lists_passes_with_no_count_to_edit(self):
        # Two PRs that each add one source each add it to the checker's EXPECTED_PATCH_SOURCES and to PATCHES_SOURCES. A count
        # typed beside the list made their two identical count edits merge into one, and main failed (#2207).
        sources = accepted_fixture_sources() + ("patch/added_by_one_pr.cpp",)
        self.assertEqual(len(sources), len(accepted_fixture_sources()) + 1)
        with tempfile.TemporaryDirectory(
            prefix="patch-source-inventory-", dir=work_root.test_scratch()
        ) as temp_dir:
            cmake = self.make_fixture(pathlib.Path(temp_dir), sources)
            with mock.patch.object(verifier, "EXPECTED_PATCH_SOURCES", sources):
                errors = verifier.verify(cmake)

        self.assertEqual(errors, [])

    def test_the_ok_line_counts_the_list_it_checked(self):
        sources = accepted_fixture_sources() + ("patch/added_by_one_pr.cpp",)
        self.assertEqual(len(sources), len(accepted_fixture_sources()) + 1)
        stdout = io.StringIO()
        with tempfile.TemporaryDirectory(
            prefix="patch-source-inventory-", dir=work_root.test_scratch()
        ) as temp_dir:
            cmake = self.make_fixture(pathlib.Path(temp_dir), sources)
            with mock.patch.object(verifier, "EXPECTED_PATCH_SOURCES", sources), mock.patch.object(
                sys, "argv", ["verify_patch_source_inventory.py", "--cmake", str(cmake)]
            ), contextlib.redirect_stdout(stdout):
                status = verifier.main()

        self.assertEqual(status, 0)
        self.assertIn(f"patch-source-inventory: OK count={len(sources)}", stdout.getvalue())

    def test_rejects_missing_duplicate_unexpected_and_missing_disk_entries(self):
        scenarios = (
            (
                "missing",
                tuple(source for source in accepted_fixture_sources() if source != "patch/xpid_patch.cpp"),
                (),
                "expected patch source absent from PATCHES_SOURCES: patch/xpid_patch.cpp",
            ),
            (
                "duplicate",
                accepted_fixture_sources() + ("patch/asset_cdn.cpp",),
                (),
                "duplicate PATCHES_SOURCES patch entry: patch/asset_cdn.cpp",
            ),
            (
                "unexpected",
                accepted_fixture_sources() + ("patch/new_runtime_patch.cpp",),
                (),
                "unexpected PATCHES_SOURCES patch source: patch/new_runtime_patch.cpp",
            ),
            (
                "missing-on-disk",
                accepted_fixture_sources(),
                ("patch/pnsrad_enabler.cpp",),
                "PATCHES_SOURCES patch source is missing from disk: patch/pnsrad_enabler.cpp",
            ),
        )

        for name, sources, absent_files, expected_error in scenarios:
            with self.subTest(name=name), tempfile.TemporaryDirectory(
                prefix="patch-source-inventory-", dir=work_root.test_scratch()
            ) as temp_dir:
                result = self.run_verifier(
                    self.make_fixture(pathlib.Path(temp_dir), sources, absent_files)
                )

            self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
            self.assertIn(expected_error, result.stderr)

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
