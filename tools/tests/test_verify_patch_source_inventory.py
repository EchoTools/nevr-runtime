#!/usr/bin/env python3
"""Regression tests for the runtime patch-source inventory verifier."""

import pathlib
import subprocess
import sys
import tempfile
import unittest


REPO = pathlib.Path(__file__).resolve().parents[2]
VERIFIER = REPO / "tools" / "verify_patch_source_inventory.py"
EXPECTED_PATCH_SOURCES = (
    "patch/asset_cdn.cpp",
    "patch/binary_bug_fixes.cpp",
    "patch/broadcaster_guard.cpp",
    "patch/broadcaster_hook_stats.cpp",
    "patch/headless_graphics.cpp",
    "patch/mode_patches.cpp",
    "patch/pnsrad_enabler.cpp",
    "patch/resource_override.cpp",
    "patch/xpid_patch.cpp",
)


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

    def test_accepts_complete_authoritative_fixture(self):
        with tempfile.TemporaryDirectory(
            prefix="patch-source-inventory-", dir="/var/tmp/work-nevr-runtime"
        ) as temp_dir:
            result = self.run_verifier(
                self.make_fixture(pathlib.Path(temp_dir), EXPECTED_PATCH_SOURCES)
            )

        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("patch-source-inventory: OK count=9", result.stdout)

    def test_rejects_missing_duplicate_unexpected_and_missing_disk_entries(self):
        scenarios = (
            (
                "missing",
                tuple(source for source in EXPECTED_PATCH_SOURCES if source != "patch/xpid_patch.cpp"),
                (),
                "expected patch source absent from PATCHES_SOURCES: patch/xpid_patch.cpp",
            ),
            (
                "duplicate",
                EXPECTED_PATCH_SOURCES + ("patch/asset_cdn.cpp",),
                (),
                "duplicate PATCHES_SOURCES patch entry: patch/asset_cdn.cpp",
            ),
            (
                "unexpected",
                EXPECTED_PATCH_SOURCES + ("patch/new_runtime_patch.cpp",),
                (),
                "unexpected PATCHES_SOURCES patch source: patch/new_runtime_patch.cpp",
            ),
            (
                "missing-on-disk",
                EXPECTED_PATCH_SOURCES,
                ("patch/pnsrad_enabler.cpp",),
                "PATCHES_SOURCES patch source is missing from disk: patch/pnsrad_enabler.cpp",
            ),
        )

        for name, sources, absent_files, expected_error in scenarios:
            with self.subTest(name=name), tempfile.TemporaryDirectory(
                prefix="patch-source-inventory-", dir="/var/tmp/work-nevr-runtime"
            ) as temp_dir:
                result = self.run_verifier(
                    self.make_fixture(pathlib.Path(temp_dir), sources, absent_files)
                )

            self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
            self.assertIn(expected_error, result.stderr)

    def test_rejects_missing_authoritative_list(self):
        with tempfile.TemporaryDirectory(
            prefix="patch-source-inventory-", dir="/var/tmp/work-nevr-runtime"
        ) as temp_dir:
            cmake = pathlib.Path(temp_dir) / "src" / "runtime" / "CMakeLists.txt"
            cmake.parent.mkdir(parents=True)
            cmake.write_text("set(OTHER_SOURCES)\n", encoding="utf-8")
            result = self.run_verifier(cmake)

        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn("PATCHES_SOURCES list not found", result.stderr)

    def test_rejects_unreadable_authoritative_file(self):
        with tempfile.TemporaryDirectory(
            prefix="patch-source-inventory-", dir="/var/tmp/work-nevr-runtime"
        ) as temp_dir:
            result = self.run_verifier(pathlib.Path(temp_dir) / "missing-CMakeLists.txt")

        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn("cannot read", result.stderr)


if __name__ == "__main__":
    unittest.main()
