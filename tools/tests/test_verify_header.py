"""The header of `just verify` names the commit, says whether the tree is clean, and lists every dirty path (#844)."""
import pathlib
import subprocess
import sys
import tempfile
import textwrap
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import just_recipe  # noqa: E402

REPO = pathlib.Path(__file__).resolve().parents[2]
JUSTFILE = REPO / "justfile"


def header_snippet() -> str:
    """The recipe's lines from the HEAD echo up to the first `just build`, de-indented."""
    text = JUSTFILE.read_text()
    body = just_recipe.body(text, "verify")
    assert body is not None, "justfile has no verify recipe"
    assert body.splitlines()[0].strip() == "#!/usr/bin/env bash", "verify recipe has no bash shebang"
    lines = body.splitlines()
    start = next(i for i, line in enumerate(lines) if 'echo "verify: HEAD' in line)
    end = next(i for i, line in enumerate(lines) if line.strip() == "just build")
    assert start < end, "the header must come before `just build`"
    return textwrap.dedent("\n".join(lines[start:end]))


class VerifyHeaderTest(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self._tmp.cleanup)
        self.repo = pathlib.Path(self._tmp.name)
        self.git("init", "-q")
        (self.repo / "tracked.txt").write_text("one\n")
        self.git("add", "tracked.txt")
        self.git("-c", "user.name=t", "-c", "user.email=t@example.invalid", "-c", "commit.gpgsign=false",
                 "commit", "-q", "-m", "c")
        self.head = self.git("rev-parse", "HEAD").strip()

    def git(self, *args: str) -> str:
        return subprocess.run(["git", *args], cwd=self.repo, check=True, capture_output=True, text=True).stdout

    def header(self) -> list[str]:
        done = subprocess.run(["bash", "-euo", "pipefail", "-c", header_snippet()], cwd=self.repo, check=True,
                              capture_output=True, text=True)
        return done.stdout.splitlines()

    def test_a_clean_tree_prints_the_head_and_clean_and_nothing_else(self):
        self.assertEqual(self.header(), [f"verify: HEAD {self.head}", "verify: tree clean"])

    def test_a_dirty_tree_prints_one_line_per_path_after_the_two_header_lines(self):
        (self.repo / "tracked.txt").write_text("two\n")
        (self.repo / "new file.txt").write_text("x\n")
        lines = self.header()
        self.assertEqual(lines[:2], [f"verify: HEAD {self.head}", "verify: tree DIRTY"])
        self.assertEqual(sorted(lines[2:]), sorted(['verify: dirty ?? "new file.txt"', "verify: dirty  M tracked.txt"]))


if __name__ == "__main__":
    unittest.main()
