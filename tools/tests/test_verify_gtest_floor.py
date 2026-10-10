"""tools/verify_gtest_floor.py: a lost runtime test fails the verify gate (#50, sprint 1).

The floor is only worth anything while it is close to the real count: with the old floor at 183 and
more than 700 tests, a hundred tests could disappear unnoticed.
"""

import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
SCRIPT = REPO / "tools" / "verify_gtest_floor.py"
JUSTFILE = REPO / "justfile"


def run(*args: str) -> subprocess.CompletedProcess:
    return subprocess.run([sys.executable, str(SCRIPT), *args], capture_output=True, text=True, cwd=REPO)


def declared_by_grep(directory: Path) -> int:
    """What the gate counted before this script: `grep -hE '^TEST(_F)?\\(' dir/*.cpp | wc -l`."""
    files = sorted(str(p) for p in directory.glob("*.cpp"))
    grep = subprocess.run(["grep", "-hE", r"^TEST(_F)?\(", *files], capture_output=True, text=True)
    return len(grep.stdout.splitlines())


class GTestFloorTest(unittest.TestCase):
    def fixture(self, files: dict) -> Path:
        directory = Path(tempfile.mkdtemp(prefix="gtest-floor-"))
        self.addCleanup(lambda: [p.unlink() for p in directory.iterdir()] or directory.rmdir())
        for name, text in files.items():
            (directory / name).write_text(text, encoding="utf-8")
        return directory

    def test_counts_test_and_test_f_declarations_at_line_start_only(self):
        directory = self.fixture({
            "a.cpp": "TEST(A, One) {}\nTEST_F(Fx, Two) {}\n  TEST(Indented, Not) {}\n// TEST(Comment, Not)\nTEST_P(P, Not) {}\n",
            "b.cpp": "TEST(B, Three) {\n}\n",
            "c.h": "TEST(Header, Not) {}\n",
        })
        result = run("--floor", "3", "--dir", str(directory))
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("declarations=3 (floor 3)", result.stdout)

    def test_fails_when_a_test_is_lost(self):
        directory = self.fixture({"a.cpp": "TEST(A, One) {}\nTEST(A, Two) {}\n"})
        result = run("--floor", "3", "--dir", str(directory))
        self.assertEqual(result.returncode, 1)
        self.assertIn("fell to 2 (floor 3)", result.stderr)

    def test_a_floor_above_the_count_by_one_fails_and_at_the_count_passes(self):
        directory = self.fixture({"a.cpp": "TEST(A, One) {}\nTEST(A, Two) {}\n"})
        self.assertEqual(run("--floor", "2", "--dir", str(directory)).returncode, 0)
        self.assertEqual(run("--floor", "3", "--dir", str(directory)).returncode, 1)

    def test_an_empty_or_missing_directory_fails_rather_than_counting_zero_against_zero(self):
        directory = self.fixture({})
        self.assertEqual(run("--floor", "0", "--dir", str(directory)).returncode, 1)
        self.assertEqual(run("--floor", "0", "--dir", str(directory / "nope")).returncode, 1)

    def test_counts_the_real_tree_exactly_as_the_old_grep_did(self):
        directory = REPO / "src" / "runtime" / "tests"
        expected = declared_by_grep(directory)
        self.assertGreater(expected, 700)
        result = run("--floor", str(expected), "--dir", str(directory))
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn(f"declarations={expected} ", result.stdout)

    def test_the_floor_in_the_justfile_is_the_real_count(self):
        """The floor equals what the gate measures on this tree, so one lost test fails it."""
        text = JUSTFILE.read_text(encoding="utf-8")
        marker = "tools/verify_gtest_floor.py --floor "
        self.assertIn(marker, text)
        floor = int(text.split(marker, 1)[1].split()[0])
        self.assertEqual(floor, declared_by_grep(REPO / "src" / "runtime" / "tests"),
                         "raise the floor in the justfile with the tests you add (a drop is a regression)")


if __name__ == "__main__":
    unittest.main()
