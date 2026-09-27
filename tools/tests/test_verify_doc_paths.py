#!/usr/bin/env python3
"""Regression coverage for documented paths absent from a fresh worktree."""

import pathlib
import shutil
import subprocess
import sys
import tempfile
import unittest


REPO = pathlib.Path(__file__).resolve().parents[2]
VERIFIER = REPO / "tools" / "verify_doc_paths.py"


class VerifyDocPathsTest(unittest.TestCase):
    def test_ignored_local_nakama_state_paths_are_optional_in_clean_checkout(self):
        """Local server state references do not make a fresh clone fail the gate."""
        with tempfile.TemporaryDirectory(
            prefix="doc-paths-clean-checkout-", dir="/var/tmp/work-nevr-runtime"
        ) as temp_dir:
            fixture = pathlib.Path(temp_dir)
            subprocess.run(["git", "init", "-q", str(fixture)], check=True)
            docs = fixture / "docs" / "reference"
            docs.mkdir(parents=True)
            document = docs / "local-nakama.md"
            document.write_text(
                "Local setup writes `tools/nakama-local/.state/nakama.yml`.\n"
                "The generated state directory is `tools/nakama-local/.state/`.\n",
                encoding="utf-8",
            )
            subprocess.run(["git", "-C", str(fixture), "add", "docs"], check=True)
            subprocess.run(
                [
                    "git", "-C", str(fixture),
                    "-c", "user.name=Doc Path Test",
                    "-c", "user.email=doc-path-test@example.invalid",
                    "commit", "-q", "-m", "fixture",
                ],
                check=True,
            )
            (fixture / "tools").mkdir()
            shutil.copy2(VERIFIER, fixture / "tools" / VERIFIER.name)

            state_dir = fixture / "tools" / "nakama-local" / ".state"
            state_file = state_dir / "nakama.yml"
            self.assertFalse(state_dir.exists())
            self.assertFalse(state_file.exists())

            result = subprocess.run(
                [sys.executable, fixture / "tools" / VERIFIER.name],
                cwd=fixture,
                capture_output=True,
                check=False,
                text=True,
            )

        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main()
