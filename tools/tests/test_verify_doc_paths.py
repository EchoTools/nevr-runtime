#!/usr/bin/env python3
"""Regression coverage for documented paths absent from a fresh worktree."""

from __future__ import annotations

import pathlib
import subprocess
import sys
import unittest

REPO = pathlib.Path(__file__).resolve().parents[2]
VERIFIER = REPO / "tools" / "verify_doc_paths.py"
sys.path.insert(0, str(REPO / "tools"))
import verify_doc_paths  # noqa: E402


class VerifyDocPathsTest(unittest.TestCase):
    def test_doc_path_gate_passes_on_current_docs(self):
        result = subprocess.run([sys.executable, str(VERIFIER)], cwd=REPO,
                                 capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("doc-paths: OK", result.stdout)


if __name__ == "__main__":
    unittest.main()
