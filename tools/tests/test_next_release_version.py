"""tools/next_release_version.py: the next release's version, computed from the Conventional Commits since the last release tag.

Run for real, as a subprocess, against a throwaway git repo built per test. The rule: docs/standards/planning.md,
"Release versions"."""
import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
SCRIPT = REPO / "tools" / "next_release_version.py"


class ReleaseVersionTest(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.repo = Path(self._tmp.name)
        self.env = dict(os.environ, GIT_AUTHOR_NAME="t", GIT_AUTHOR_EMAIL="t@example.invalid",
                        GIT_COMMITTER_NAME="t", GIT_COMMITTER_EMAIL="t@example.invalid",
                        GIT_CONFIG_GLOBAL=os.devnull, GIT_CONFIG_NOSYSTEM="1")
        self.git("init", "-q", "-b", "main")
        self.commit("chore: the first commit")
        self.git("tag", "-a", "v5.5.1", "-m", "5.5.1")

    def tearDown(self):
        self._tmp.cleanup()

    def git(self, *args):
        subprocess.run(["git", "-C", str(self.repo), *args], check=True, env=self.env, capture_output=True)

    def commit(self, message):
        self.git("commit", "-q", "--allow-empty", "-m", message)

    def run_tool(self, *args):
        return subprocess.run([sys.executable, "-I", str(SCRIPT), "--repo", str(self.repo), *args],
                              capture_output=True, text=True)

    def assertVersion(self, expected):
        result = self.run_tool()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout, expected + "\n")
        return result

    def test_a_fix_bumps_the_patch(self):
        self.commit("fix(boot): accept --server (#1)")
        self.assertVersion("v5.5.2")

    def test_a_perf_bumps_the_patch(self):
        self.commit("perf(log): fewer allocations (#2)")
        self.assertVersion("v5.5.2")

    def test_a_feat_bumps_the_minor_and_resets_the_patch(self):
        self.commit("fix(boot): accept --server (#1)")
        self.commit("feat(plugins): plugin API v7 (#3)")
        self.assertVersion("v5.6.0")

    def test_a_bang_bumps_the_major(self):
        self.commit("feat(cli)!: drop the old flag names (#4)")
        self.assertVersion("v6.0.0")

    def test_a_breaking_change_footer_bumps_the_major(self):
        self.commit("refactor(host): split the host\n\nBREAKING CHANGE: the runtime is nevr-runtime.dll")
        self.assertVersion("v6.0.0")

    def test_breaking_change_text_inside_a_body_line_is_not_a_footer(self):
        self.commit("fix(docs): say what a BREAKING CHANGE: footer is\n\nWe explain BREAKING CHANGE: here.")
        self.assertVersion("v5.5.2")

    def test_the_type_is_case_insensitive(self):
        self.commit("Feat(ui): a thing (#5)")
        self.assertVersion("v5.6.0")

    def test_only_docs_and_chores_is_nothing_to_release(self):
        self.commit("docs: a typo (#6)")
        self.commit("ci: a workflow (#7)")
        result = self.run_tool()
        self.assertEqual(result.returncode, 1, result.stdout)
        self.assertEqual(result.stdout, "")
        self.assertIn("nothing to release", result.stderr)

    def test_the_base_is_the_newest_release_tag_not_a_scratch_or_prerelease_tag(self):
        self.commit("feat: shipped in 5.6.0 (#8)")
        self.git("tag", "-a", "v5.6.0", "-m", "5.6.0")
        self.commit("fix: after 5.6.0 (#9)")
        self.git("tag", "-a", "v5.7.0-rc.1", "-m", "rc")
        self.git("tag", "-a", "v0.0.901", "-m", "scratch dry run")
        self.assertVersion("v5.6.1")

    def test_every_counted_commit_is_logged_with_its_bump(self):
        self.commit("fix(boot): accept --server (#1)")
        self.commit("docs: a typo (#6)")
        result = self.assertVersion("v5.5.2")
        lines = result.stderr.strip().splitlines()
        self.assertTrue(any("base=v5.5.1" in line for line in lines), result.stderr)
        sha = subprocess.run(["git", "-C", str(self.repo), "rev-parse", "--short=12", "HEAD~1"], capture_output=True,
                             text=True, check=True, env=self.env).stdout.strip()
        self.assertTrue(any(f"commit={sha} bump=patch subject=fix(boot): accept --server (#1)" in line for line in lines),
                        result.stderr)
        self.assertTrue(any("bump=none" in line and "docs: a typo (#6)" in line for line in lines), result.stderr)

    def test_no_release_tag_is_an_error(self):
        self.git("tag", "-d", "v5.5.1")
        result = self.run_tool()
        self.assertEqual(result.returncode, 2)
        self.assertIn("no release tag", result.stderr)

    def test_a_path_that_is_not_a_repo_is_an_error(self):
        with tempfile.TemporaryDirectory() as empty:
            result = subprocess.run([sys.executable, "-I", str(SCRIPT), "--repo", empty], capture_output=True, text=True)
        self.assertEqual(result.returncode, 2)


if __name__ == "__main__":
    unittest.main()
