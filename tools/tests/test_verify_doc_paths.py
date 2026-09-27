"""The document path sensor allows only intentional, generated local state."""

from __future__ import annotations

import pathlib
import subprocess
import sys
import unittest

REPO = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "tools"))
import verify_doc_paths  # noqa: E402


class GeneratedLocalStateTest(unittest.TestCase):
    def test_nakama_state_references_are_allowed_because_setup_generates_gitignored_files(self):
        checker = (REPO / "tools/verify_doc_paths.py").read_text()
        allowlist = {
            "tools/nakama-local/.state",
            "tools/nakama-local/.state/nakama.yml",
        }
        self.assertTrue(allowlist.issubset(verify_doc_paths.ALLOWED_ABSENT))
        self.assertIn('Writes tools/nakama-local/.state/ (git-ignored)',
                      (REPO / "tools/nakama-local/setup.py").read_text())
        self.assertIn(".state/", (REPO / "tools/nakama-local/.gitignore").read_text())
        self.assertIn("local Nakama rig creates these machine-local files", checker)

    def test_doc_path_gate_passes_without_optional_nakama_setup(self):
        result = subprocess.run(["python3", str(REPO / "tools/verify_doc_paths.py")],
                                cwd=REPO, capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("doc-paths: OK", result.stdout)


if __name__ == "__main__":
    unittest.main()
