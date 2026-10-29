"""tools/verify_gtest_floor.py: a lost runtime test fails the gate, against the count git has at the base (#50, #1343).

The floor is not stored: it is the count at the merge-base with origin/main (on main, the parent commit), so a
pull request that adds tests edits nothing to pass, and one that drops tests says so in a GTest-Drop trailer.
"""

import os
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
SCRIPT = REPO / "tools" / "verify_gtest_floor.py"
JUSTFILE = REPO / "justfile"
GIT_ENV = {**os.environ, "GIT_AUTHOR_NAME": "t", "GIT_AUTHOR_EMAIL": "t@t", "GIT_COMMITTER_NAME": "t",
           "GIT_COMMITTER_EMAIL": "t@t", "GIT_CONFIG_GLOBAL": os.devnull, "GIT_CONFIG_NOSYSTEM": "1"}


def declared_by_grep(directory: Path) -> int:
    """What the gate counted before this script: `grep -hE '^TEST(_F)?\\(' dir/*.cpp | wc -l`."""
    files = sorted(str(p) for p in directory.glob("*.cpp"))
    grep = subprocess.run(["grep", "-hE", r"^TEST(_F)?\(", *files], capture_output=True, text=True)
    return len(grep.stdout.splitlines())


class GTestBaseTest(unittest.TestCase):
    def setUp(self):
        self.repo = Path(tempfile.mkdtemp(prefix="gtest-base-"))
        self.addCleanup(shutil.rmtree, self.repo)
        self.git("init", "-q", "-b", "main")
        self.tests = self.repo / "src" / "runtime" / "tests"
        self.tests.mkdir(parents=True)

    def git(self, *args: str) -> str:
        return subprocess.run(["git", "-C", str(self.repo), *args], capture_output=True, text=True, check=True,
                              env=GIT_ENV).stdout

    def commit(self, files: dict, message: str = "c") -> str:
        for name, text in files.items():
            path = self.tests / name
            if text is None:
                path.unlink()
            else:
                path.write_text(text, encoding="utf-8")
        self.git("add", "-A")
        self.git("commit", "-q", "-m", message)
        return self.git("rev-parse", "HEAD").strip()

    def mark_origin_main(self):
        self.git("update-ref", "refs/remotes/origin/main", "HEAD")

    def run_gate(self, *args: str) -> subprocess.CompletedProcess:
        return subprocess.run([sys.executable, str(SCRIPT), "--repo", str(self.repo), *args],
                              capture_output=True, text=True, env=GIT_ENV)

    def test_counts_test_and_test_f_declarations_at_line_start_only(self):
        self.commit({"a.cpp": "TEST(A, One) {}\nTEST_F(Fx, Two) {}\n  TEST(Indented, Not) {}\n// TEST(Comment, Not)\n"
                              "TEST_P(P, Not) {}\n",
                     "b.cpp": "TEST(B, Three) {\n}\n", "c.h": "TEST(Header, Not) {}\n"})
        self.mark_origin_main()
        self.commit({"d.cpp": "TEST(D, Four) {}\n"})
        result = self.run_gate()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("declarations=4 ", result.stdout)
        self.assertIn("had 3)", result.stdout)

    def test_a_branch_that_adds_tests_passes_with_no_stored_number(self):
        self.commit({"a.cpp": "TEST(A, One) {}\n"})
        self.mark_origin_main()
        self.commit({"a.cpp": "TEST(A, One) {}\nTEST(A, Two) {}\n"})
        self.assertEqual(self.run_gate().returncode, 0)

    def test_a_branch_that_loses_a_test_fails_against_the_merge_base(self):
        self.commit({"a.cpp": "TEST(A, One) {}\nTEST(A, Two) {}\n"})
        self.mark_origin_main()
        self.commit({"a.cpp": "TEST(A, One) {}\n"})
        result = self.run_gate()
        self.assertEqual(result.returncode, 1)
        self.assertIn("fell to 1", result.stderr)
        self.assertIn("had 2", result.stderr)

    def test_the_base_is_the_merge_base_not_origin_main_s_tip(self):
        self.commit({"a.cpp": "TEST(A, One) {}\n"})
        base = self.git("rev-parse", "HEAD").strip()
        self.commit({"a.cpp": "TEST(A, One) {}\nTEST(A, Two) {}\nTEST(A, Three) {}\n"})
        self.mark_origin_main()  # main moved on with more tests
        self.git("checkout", "-q", "-b", "topic", base)
        self.commit({"b.cpp": "TEST(B, One) {}\n"})
        result = self.run_gate()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn(f"base {base[:12]} had 1", result.stdout)

    def test_on_main_the_base_is_the_parent_commit(self):
        self.commit({"a.cpp": "TEST(A, One) {}\nTEST(A, Two) {}\n"})
        self.commit({"a.cpp": "TEST(A, One) {}\n"})
        self.mark_origin_main()  # HEAD is origin/main: the merge-base is HEAD itself
        result = self.run_gate()
        self.assertEqual(result.returncode, 1)
        self.assertIn("had 2", result.stderr)

    def test_a_drop_trailer_lets_a_declared_removal_pass_and_is_printed(self):
        self.commit({"a.cpp": "TEST(A, One) {}\nTEST(A, Two) {}\n"})
        self.mark_origin_main()
        self.commit({"a.cpp": "TEST(A, One) {}\n"}, "test: drop a duplicate\n\nGTest-Drop: A.Two duplicated A.One")
        result = self.run_gate()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("GTest-Drop: A.Two duplicated A.One", result.stdout)

    def test_a_trailer_before_the_base_does_not_excuse_a_later_drop(self):
        self.commit({"a.cpp": "TEST(A, One) {}\nTEST(A, Two) {}\n"}, "t\n\nGTest-Drop: old reason")
        self.mark_origin_main()
        self.commit({"a.cpp": "TEST(A, One) {}\n"})
        self.assertEqual(self.run_gate().returncode, 1)

    def test_an_unreadable_base_fails_loudly(self):
        self.commit({"a.cpp": "TEST(A, One) {}\n"})  # one commit, no origin/main: no merge-base
        result = self.run_gate()
        self.assertEqual(result.returncode, 1)
        self.assertIn("base commit's GTest count cannot be read", result.stderr)

    def test_an_empty_test_directory_fails_rather_than_counting_zero_against_zero(self):
        self.commit({"a.cpp": "TEST(A, One) {}\n"})
        self.mark_origin_main()
        self.commit({"a.cpp": None, "keep.txt": "x"})
        self.assertEqual(self.run_gate().returncode, 1)

    def test_counts_the_real_tree_exactly_as_the_old_grep_did(self):
        expected = declared_by_grep(REPO / "src" / "runtime" / "tests")
        self.assertGreater(expected, 700)
        result = subprocess.run([sys.executable, str(SCRIPT), "--base", "HEAD"], capture_output=True, text=True,
                                cwd=REPO)
        self.assertIn(f"declarations={expected} ", result.stdout + result.stderr)

    def test_the_justfile_stores_no_floor(self):
        text = JUSTFILE.read_text(encoding="utf-8")
        self.assertIn("python3 tools/verify_gtest_floor.py\n", text)
        self.assertNotIn("verify_gtest_floor.py --floor", text)


if __name__ == "__main__":
    unittest.main()
