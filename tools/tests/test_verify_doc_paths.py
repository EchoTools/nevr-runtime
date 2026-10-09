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

    def test_source_comment_citation_to_a_missing_document_is_reported(self):
        files = {"a.cpp": "// see docs/design/gone.md for why\nint x;\n"}
        bad = verify_doc_paths.bad_source_refs(files, read=files.__getitem__, exists=lambda p: False)
        self.assertEqual(bad, [("a.cpp", 1, "docs/design/gone.md")])

    def test_source_comment_citation_to_an_existing_document_passes(self):
        files = {"a.h": "/// spec: docs/adr/0005-x.md\n"}
        bad = verify_doc_paths.bad_source_refs(files, read=files.__getitem__, exists=lambda p: True)
        self.assertEqual(bad, [])

    def test_other_repository_reference_is_not_checked_here(self):
        text = "// (echovr-reconstruction docs/earlyquit_field_analysis.md, from ReVault)\n"
        self.assertEqual(list(verify_doc_paths.source_doc_refs(text)), [])
        # The marker only exempts what follows it on the same line.
        self.assertEqual(list(verify_doc_paths.source_doc_refs("// docs/a.md and echovr-reconstruction\n")),
                         [(1, "docs/a.md")])

    def test_every_tracked_source_citation_resolves_now(self):
        self.assertEqual(verify_doc_paths.bad_source_refs(verify_doc_paths.tracked_source_files()), [])


if __name__ == "__main__":
    unittest.main()
