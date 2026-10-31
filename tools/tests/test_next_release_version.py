"""tools/next_release_version.py: the next release's version, computed from the Conventional Commits since the last release tag.

Run for real, as a subprocess, against a throwaway git repo built per test. The rule: docs/standards/planning.md,
"Release versions"."""
import json
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

    def test_a_bang_on_an_unscoped_feat_bumps_the_major(self):
        self.commit("feat!: drop the old flag names (#4)")
        self.assertVersion("v6.0.0")

    def test_a_breaking_change_footer_bumps_the_major(self):
        self.commit("feat(host): split the host\n\nBREAKING CHANGE: the runtime is nevr-runtime.dll")
        self.assertVersion("v6.0.0")

    def test_a_fix_with_a_breaking_change_footer_bumps_the_major(self):
        self.commit("fix(host): split the host\n\nBREAKING CHANGE: the runtime is nevr-runtime.dll")
        self.assertVersion("v6.0.0")

    def test_a_bang_on_a_chore_bumps_nothing(self):
        self.commit("chore!: reference nothing that only one machine has (#4)")
        result = self.run_tool()
        self.assertEqual(result.returncode, 1, result.stdout)
        self.assertEqual(result.stdout, "")
        self.assertIn('"event": "nothing_to_release"', result.stderr)

    def test_a_breaking_change_footer_on_a_chore_bumps_nothing(self):
        self.commit("chore: drop a file\n\nBREAKING CHANGE: the file is gone")
        result = self.run_tool()
        self.assertEqual(result.returncode, 1, result.stdout)
        self.assertIn('"event": "nothing_to_release"', result.stderr)

    def test_a_bang_on_a_scoped_docs_bumps_nothing(self):
        self.commit("docs(scope)!: rewrite the release section (#4)")
        result = self.run_tool()
        self.assertEqual(result.returncode, 1, result.stdout)
        self.assertIn('"event": "nothing_to_release"', result.stderr)

    def test_a_breaking_footer_on_a_refactor_bumps_nothing_and_a_later_feat_bumps_the_minor(self):
        self.commit("refactor(host): split the host\n\nBREAKING CHANGE: the runtime is nevr-runtime.dll")
        result = self.run_tool()
        self.assertEqual(result.returncode, 1, result.stdout)
        self.commit("feat: a later feature (#5)")
        self.assertVersion("v5.6.0")

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
        self.assertIn('"event": "nothing_to_release"', result.stderr)

    def test_the_base_is_the_newest_release_tag_not_a_scratch_or_prerelease_tag(self):
        self.commit("feat: shipped in 5.6.0 (#8)")
        self.git("tag", "-a", "v5.6.0", "-m", "5.6.0")
        self.commit("fix: after 5.6.0 (#9)")
        self.git("tag", "-a", "v5.7.0-rc.1", "-m", "rc")
        self.git("tag", "-a", "v0.0.901", "-m", "scratch dry run")
        self.assertVersion("v5.6.1")

    def test_every_commit_is_logged_with_its_bump_as_json(self):
        self.commit("fix(boot): accept --server (#1)")
        self.commit("docs: a typo (#6)")
        result = self.assertVersion("v5.5.2")
        records = [json.loads(line) for line in result.stderr.strip().splitlines()]
        self.assertTrue(all(r["tool"] == "next_release_version" for r in records), result.stderr)
        self.assertEqual(records[0], {"tool": "next_release_version", "event": "base", "base": "v5.5.1", "rev": "HEAD"})
        sha = subprocess.run(["git", "-C", str(self.repo), "rev-parse", "--short=12", "HEAD~1"], capture_output=True,
                             text=True, check=True, env=self.env).stdout.strip()
        self.assertIn({"tool": "next_release_version", "event": "commit", "commit": sha, "bump": "patch",
                       "subject": "fix(boot): accept --server (#1)"}, records)
        self.assertTrue(any(r.get("bump") == "none" and r.get("subject") == "docs: a typo (#6)" for r in records),
                        result.stderr)
        self.assertEqual(records[-1], {"tool": "next_release_version", "event": "next", "bump": "patch",
                                       "next": "v5.5.2"})

    def test_a_hyphenated_breaking_change_footer_bumps_the_major(self):
        self.commit("feat(host): split the host\n\nBREAKING-CHANGE: the runtime is nevr-runtime.dll")
        self.assertVersion("v6.0.0")

    def test_sub_commit_subjects_in_a_squash_body_give_no_type(self):
        self.commit("docs(planning): the squash title (#7)\n\n* feat!: a sub-commit\n\n* fix: another sub-commit")
        result = self.run_tool()
        self.assertEqual(result.returncode, 1, result.stderr)
        self.assertIn('"event": "nothing_to_release"', result.stderr)

    def test_a_first_line_with_no_type_gives_none_whatever_the_body_says(self):
        self.commit("Merge pull request #7 from a/branch\n\nfeat!: the branch's subject")
        result = self.run_tool()
        self.assertEqual(result.returncode, 1, result.stderr)

    def test_a_breaking_change_footer_in_a_squash_body_bumps_the_major(self):
        self.commit("fix(host): the squash title (#7)\n\n* refactor: a sub-commit\n\nBREAKING CHANGE: the export moved")
        self.assertVersion("v6.0.0")

    def test_a_merged_branch_counts_and_an_unmerged_tag_is_not_the_base(self):
        self.git("checkout", "-q", "-b", "side")
        self.commit("feat: on the side branch (#3)")
        self.git("checkout", "-q", "main")
        self.git("checkout", "-q", "-b", "other")
        self.commit("fix: on an unmerged branch (#4)")
        self.git("tag", "-a", "v9.0.0", "-m", "unreachable")
        self.git("checkout", "-q", "main")
        self.commit("docs: on main (#5)")
        self.git("merge", "-q", "--no-ff", "-m", "Merge branch side", "side")
        self.assertVersion("v5.6.0")

    def test_a_computed_tag_that_already_exists_is_an_error(self):
        self.git("checkout", "-q", "-b", "other")
        self.commit("fix: released from another line (#4)")
        self.git("tag", "-a", "v5.5.2", "-m", "5.5.2")
        self.git("checkout", "-q", "main")
        self.commit("fix: on main (#5)")
        result = self.run_tool()
        self.assertEqual(result.returncode, 2, result.stderr)
        self.assertIn("v5.5.2 already exists", result.stderr)
        self.assertEqual(result.stdout, "")

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
