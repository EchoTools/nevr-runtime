"""tools/check_pr_read_line.py: a PR body ends with `Read AGENTS.md at <sha of the PR's merge-base>`.

A tell for "did the agent read the contract", not a security control. The script is run for real, as
a subprocess, against a throwaway git repository."""
import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
SCRIPT = REPO / "tools" / "check_pr_read_line.py"


def git(cwd, *args):
    env = {**os.environ, "GIT_AUTHOR_NAME": "t", "GIT_AUTHOR_EMAIL": "t@example.invalid",
           "GIT_COMMITTER_NAME": "t", "GIT_COMMITTER_EMAIL": "t@example.invalid"}
    return subprocess.run(["git", "-C", str(cwd), *args], check=True, capture_output=True, text=True,
                          env=env).stdout.strip()


class CheckPrReadLineTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls._tmp = tempfile.TemporaryDirectory(prefix="read-line-")
        cls.repo = Path(cls._tmp.name)
        git(cls.repo, "init", "-q", "-b", "main")
        (cls.repo / "f").write_text("0\n")
        git(cls.repo, "add", "f")
        git(cls.repo, "commit", "-q", "--no-gpg-sign", "-m", "root")
        cls.root = git(cls.repo, "rev-parse", "HEAD")
        git(cls.repo, "checkout", "-q", "-b", "topic")
        (cls.repo / "t").write_text("t\n")
        git(cls.repo, "add", "t")
        git(cls.repo, "commit", "-q", "--no-gpg-sign", "-m", "topic work")
        cls.head = git(cls.repo, "rev-parse", "HEAD")
        git(cls.repo, "checkout", "-q", "main")
        (cls.repo / "f").write_text("1\n")
        git(cls.repo, "commit", "-q", "--no-gpg-sign", "-am", "main moves after the PR was opened")
        cls.main_tip = git(cls.repo, "rev-parse", "HEAD")

    @classmethod
    def tearDownClass(cls):
        cls._tmp.cleanup()

    def run_check(self, body, base=None, head=None, via="env"):
        base = base or self.main_tip
        head = head or self.head
        argv = [sys.executable, str(SCRIPT), "--base-sha", base, "--head-sha", head]
        env = {k: v for k, v in os.environ.items() if k != "PR_BODY"}
        if via == "env":
            env["PR_BODY"] = body
        else:
            bodyfile = self.repo / "body.txt"
            bodyfile.write_text(body)
            argv += ["--body-file", str(bodyfile)]
        return subprocess.run(argv, cwd=self.repo, env=env, capture_output=True, text=True)

    def assertFails(self, result, needle):
        self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
        lines = result.stderr.strip().splitlines()
        self.assertEqual(len(lines), 1, f"one clear line on failure, got: {result.stderr!r}")
        self.assertIn(needle, lines[0])

    def test_passes_on_the_merge_base_sha(self):
        for abbrev in (self.root, self.root[:12], self.root[:7]):
            with self.subTest(abbrev=abbrev):
                result = self.run_check(f"What\n\nBranch lifecycle: x\nRead AGENTS.md at {abbrev}\n")
                self.assertEqual(result.returncode, 0, result.stderr)

    def test_passes_through_a_body_file_and_with_crlf_and_trailing_blank_lines(self):
        body = f"What\r\n\r\nRead AGENTS.md at {self.root[:9]}\r\n\r\n\r\n"
        result = self.run_check(body, via="file")
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_fails_on_a_body_without_the_line(self):
        result = self.run_check("What this does.\n\nBranch lifecycle: x\n")
        self.assertFails(result, "missing")

    def test_fails_on_an_empty_body(self):
        self.assertFails(self.run_check(""), "missing")

    def test_fails_when_the_line_is_not_the_last_non_empty_line(self):
        body = f"Read AGENTS.md at {self.root}\n\nBranch lifecycle: x\n"
        self.assertFails(self.run_check(body), "not the last")

    def test_fails_on_a_wrong_sha(self):
        for wrong in (self.head, self.main_tip, self.head[:10]):
            with self.subTest(wrong=wrong):
                result = self.run_check(f"What\n\nRead AGENTS.md at {wrong}\n")
                self.assertFails(result, "is not the PR's base")

    def test_fails_on_a_sha_that_resolves_to_no_commit(self):
        self.assertFails(self.run_check("x\n\nRead AGENTS.md at deadbeefdeadbeef\n"), "is not the PR's base")

    def test_fails_on_an_abbreviation_shorter_than_seven(self):
        self.assertFails(self.run_check(f"x\n\nRead AGENTS.md at {self.root[:6]}\n"), "malformed")

    def test_fails_on_a_non_hex_value_or_a_wrong_sentence(self):
        for line in ("Read AGENTS.md at HEAD", "Read AGENTS.md at origin/main", "Read AGENTS.md", f"I read AGENTS.md at {self.root}",
                     f"Read AGENTS.md at {self.root} thanks"):
            with self.subTest(line=line):
                self.assertNotEqual(self.run_check(f"x\n\n{line}\n").returncode, 0)

    def test_the_base_is_the_merge_base_not_the_target_tip(self):
        """Main moved after the PR was opened: the PR's base stays the commit it branched from, and the
        tip of main is a wrong answer."""
        self.assertNotEqual(self.root, self.main_tip)
        self.assertEqual(self.run_check(f"x\n\nRead AGENTS.md at {self.root[:7]}\n").returncode, 0)
        self.assertNotEqual(self.run_check(f"x\n\nRead AGENTS.md at {self.main_tip[:7]}\n").returncode, 0)

    def test_an_unreadable_range_fails_loud(self):
        result = self.run_check(f"x\n\nRead AGENTS.md at {self.root}\n", base="0" * 40)
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(len(result.stderr.strip().splitlines()), 1, result.stderr)


if __name__ == "__main__":
    unittest.main()
