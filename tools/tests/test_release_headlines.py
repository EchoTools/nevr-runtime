"""The PR-time gate on the single shared headline file (#1482): `release-headline.txt` must not exist in the tree. A PR
written for the single-file rule adds it, and `package_release.py notes` refuses it only at step 7 of the cut, after the tag
and the draft exist; this test fails the PR instead. Headlines live in `release-headlines/<topic>.txt`."""
import sys
import tempfile
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "tools"))

import package_release  # noqa: E402


class SharedHeadlineFileGate(unittest.TestCase):
    def test_the_tree_has_no_shared_headline_file(self):
        problem = package_release.shared_headline_file_problem(REPO)
        self.assertIsNone(problem, problem)

    def test_a_tree_that_holds_the_shared_file_is_a_problem_that_names_the_replacement(self):
        with tempfile.TemporaryDirectory() as root:
            (Path(root) / "release-headline.txt").write_text("Old rule.\n", encoding="utf-8")
            problem = package_release.shared_headline_file_problem(Path(root))
        self.assertIn("release-headline.txt", problem)
        self.assertIn("release-headlines/<topic>.txt", problem)
        self.assertIn("release-headlines/README.md", problem)

    def test_a_tree_without_the_shared_file_is_no_problem(self):
        with tempfile.TemporaryDirectory() as root:
            (Path(root) / "release-headlines").mkdir()
            (Path(root) / "release-headlines" / "a.txt").write_text("New.\n", encoding="utf-8")
            self.assertIsNone(package_release.shared_headline_file_problem(Path(root)))


if __name__ == "__main__":
    unittest.main()
