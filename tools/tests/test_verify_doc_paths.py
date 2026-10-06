#!/usr/bin/env python3
"""Regression coverage for documented paths absent from a fresh worktree."""

from __future__ import annotations

import pathlib
import shutil
import subprocess
import sys
import tempfile
import unittest

REPO = pathlib.Path(__file__).resolve().parents[2]
VERIFIER = REPO / "tools" / "verify_doc_paths.py"
sys.path.insert(0, str(REPO / "tools"))
import verify_doc_paths  # noqa: E402


class VerifyDocPathsTest(unittest.TestCase):
    def test_ignored_optional_dependency_paths_are_optional_in_clean_checkout(self):
        """Documented machine-local dependencies do not make a fresh clone fail the gate."""
        scratch = pathlib.Path("/var/tmp/work-nevr-runtime")
        scratch.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(prefix="doc-paths-clean-checkout-", dir=scratch) as temp_dir:
            fixture = pathlib.Path(temp_dir)
            subprocess.run(["git", "init", "-q", str(fixture)], check=True)
            docs = fixture / "docs" / "reference"
            docs.mkdir(parents=True)
            (docs / "optional-dependency.md").write_text(
                "The generated dependency lives at `extern/protobuf`.\n",
                encoding="utf-8",
            )
            subprocess.run(["git", "-C", str(fixture), "add", "docs"], check=True)
            subprocess.run(
                ["git", "-C", str(fixture), "-c", "user.name=Doc Path Test",
                 "-c", "user.email=doc-path-test@example.invalid", "commit", "-q", "-m", "fixture"],
                check=True,
            )
            (fixture / "tools").mkdir()
            shutil.copy2(VERIFIER, fixture / "tools" / VERIFIER.name)
            state_dir = fixture / "extern" / "protobuf"
            state_file = state_dir / "include" / "protobuf.h"
            self.assertFalse(state_dir.exists())
            self.assertFalse(state_file.exists())
            result = subprocess.run(
                [sys.executable, fixture / "tools" / VERIFIER.name], cwd=fixture,
                capture_output=True, check=False, text=True,
            )
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_doc_path_gate_passes_without_optional_dependency(self):
        result = subprocess.run([sys.executable, str(VERIFIER)], cwd=REPO,
                                 capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("doc-paths: OK", result.stdout)


if __name__ == "__main__":
    unittest.main()
