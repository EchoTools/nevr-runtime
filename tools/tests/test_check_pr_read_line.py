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
        # Return code 1 is the script's own failure; 2 (argparse) or 127/other means it did not run.
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertEqual(result.stdout, "", "a failure prints nothing on stdout")
        lines = result.stderr.strip().splitlines()
        self.assertEqual(len(lines), 1, f"one clear line on failure, got: {result.stderr!r}")
        self.assertTrue(lines[0].startswith("FAIL: "), f"the failure line starts with `FAIL: `: {lines[0]!r}")
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
        for line, needle in (("Read AGENTS.md at HEAD", "malformed sha 'HEAD'"),
                             ("Read AGENTS.md at origin/main", "malformed sha 'origin/main'"),
                             ("Read AGENTS.md", "missing"),
                             (f"I read AGENTS.md at {self.root}", "missing"),
                             (f"Read AGENTS.md at {self.root} thanks", "malformed last line")):
            with self.subTest(line=line):
                self.assertFails(self.run_check(f"x\n\n{line}\n"), needle)

    def test_the_base_is_the_merge_base_not_the_target_tip(self):
        """Main moved after the PR was opened: the PR's base stays the commit it branched from, and the
        tip of main is a wrong answer."""
        self.assertNotEqual(self.root, self.main_tip)
        self.assertEqual(self.run_check(f"x\n\nRead AGENTS.md at {self.root[:7]}\n").returncode, 0)
        self.assertFails(self.run_check(f"x\n\nRead AGENTS.md at {self.main_tip[:7]}\n"), "is not the PR's base")

    def test_an_unreadable_range_fails_loud(self):
        result = self.run_check(f"x\n\nRead AGENTS.md at {self.root}\n", base="0" * 40)
        self.assertFails(result, "cannot compute the merge-base")

    def test_a_non_full_sha_argument_fails_loud(self):
        self.assertFails(self.run_check(f"x\n\nRead AGENTS.md at {self.root}\n", base=self.main_tip[:12]),
                         "is not a full 40-hex commit sha")

    def test_a_prefix_that_git_reads_as_another_object_says_so(self):
        """A tag named like the first hex characters of the base wins over the object name in rev-parse.
        The text IS a prefix of the base, so the failure must say it is ambiguous, not that it is
        'not the PR's base commit'."""
        with tempfile.TemporaryDirectory(prefix="read-line-tag-") as tmp:
            repo = Path(tmp)
            git(repo, "init", "-q", "-b", "main")
            git(repo, "commit", "-q", "--no-gpg-sign", "--allow-empty", "-m", "base")
            base = git(repo, "rev-parse", "HEAD")
            git(repo, "commit", "-q", "--no-gpg-sign", "--allow-empty", "-m", "topic")
            head = git(repo, "rev-parse", "HEAD")
            git(repo, "update-ref", f"refs/tags/{base[:8]}", head)
            self.assertEqual(git(repo, "rev-parse", f"{base[:8]}^{{commit}}"), head)  # the tag wins
            env = {k: v for k, v in os.environ.items() if k != "PR_BODY"}
            env["PR_BODY"] = f"x\n\nRead AGENTS.md at {base[:8]}\n"
            result = subprocess.run([sys.executable, str(SCRIPT), "--base-sha", base, "--head-sha", head],
                                    cwd=repo, env=env, capture_output=True, text=True)
            self.assertFails(result, "is a prefix of the PR's base commit")
            self.assertIn("ambiguous", result.stderr)
            self.assertNotIn("is not the PR's base commit", result.stderr)
            # The full sha is not shadowed by the tag, so it still passes.
            env["PR_BODY"] = f"x\n\nRead AGENTS.md at {base}\n"
            ok = subprocess.run([sys.executable, str(SCRIPT), "--base-sha", base, "--head-sha", head],
                                cwd=repo, env=env, capture_output=True, text=True)
            self.assertEqual(ok.returncode, 0, ok.stderr)


if __name__ == "__main__":
    unittest.main()
