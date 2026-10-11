"""tools/release_preflight.py (`just release-preflight`): a release is only started from a pristine clone.

Real git in scratch repositories: a bare "origin" and a clone of it. Each refusal has its own test; the
pristine clone passes; and the check is read-only: no fetch, pull, push, tag, checkout or reset is ever run.
"""
from __future__ import annotations

import json
import os
import pathlib
import shutil
import stat
import subprocess
import tempfile
import unittest

REPO = pathlib.Path(__file__).resolve().parents[2]
SCRIPT = REPO / "tools" / "release_preflight.py"


def git(cwd, *args, env=None):
    return subprocess.run(["git", "-c", "user.name=t", "-c", "user.email=t@example.invalid",
                           "-c", "commit.gpgsign=false", "-c", "tag.gpgsign=false", *args],
                          cwd=cwd, check=True, capture_output=True, text=True, env=env)


@unittest.skipUnless(shutil.which("git"), "git is required")
class ReleasePreflightTest(unittest.TestCase):
    def setUp(self):
        self.tmp = pathlib.Path(tempfile.mkdtemp(prefix="release-preflight-", dir="/var/tmp"))
        self.addCleanup(shutil.rmtree, self.tmp)
        self.origin = self.tmp / "origin.git"
        git(self.tmp, "init", "-q", "--bare", "-b", "main", str(self.origin))
        seed = self.tmp / "seed"
        git(self.tmp, "clone", "-q", str(self.origin), str(seed))
        git(seed, "switch", "-q", "-c", "main")
        (seed / "a.txt").write_text("one\n")
        git(seed, "add", "a.txt")
        git(seed, "commit", "-q", "-m", "one")
        git(seed, "tag", "-a", "-m", "v3.3.0", "v3.3.0")
        git(seed, "push", "-q", "origin", "main", "v3.3.0")
        self.seed = seed
        self.clone = self.tmp / "clone"
        git(self.tmp, "clone", "-q", str(self.origin), str(self.clone))
        self.log = self.tmp / "preflight.jsonl"

    def run_preflight(self, *extra, env=None, cwd=None):
        e = dict(os.environ, RELEASE_PREFLIGHT_LOG=str(self.log))
        if env:
            e.update(env)
        return subprocess.run([str(SCRIPT), *extra], cwd=cwd or self.clone, env=e, capture_output=True, text=True)

    def problems(self, result):
        return [line for line in result.stdout.splitlines() if line.startswith("release-preflight: PROBLEM:")]

    def push_from_seed(self, name="two"):
        (self.seed / f"{name}.txt").write_text(name)
        git(self.seed, "add", f"{name}.txt")
        git(self.seed, "commit", "-q", "-m", name)
        git(self.seed, "push", "-q", "origin", "main")
        return git(self.seed, "rev-parse", "HEAD").stdout.strip()

    # --- the pass -----------------------------------------------------------------------------------

    def test_a_pristine_clone_on_origins_tip_passes(self):
        result = self.run_preflight()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("release-preflight: OK", result.stdout)
        self.assertEqual(self.problems(result), [])

    def test_an_ignored_file_does_not_refuse(self):
        (self.clone / ".gitignore").write_text("secret.env\n")
        git(self.clone, "add", ".gitignore")
        git(self.clone, "commit", "-q", "-m", "ignore")
        git(self.clone, "push", "-q", "origin", "main")
        (self.clone / "secret.env").write_text("x\n")
        result = self.run_preflight()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    # --- each refusal -------------------------------------------------------------------------------

    def test_a_local_only_tag_is_refused_and_named(self):
        git(self.clone, "tag", "-a", "-m", "v4.0.0", "v4.0.0")
        result = self.run_preflight()
        self.assertEqual(result.returncode, 1)
        self.assertEqual(len(self.problems(result)), 1, result.stdout)
        self.assertIn("v4.0.0", result.stdout)
        self.assertIn("only in this clone", result.stdout)

    def test_a_lightweight_local_only_tag_is_refused_too(self):
        git(self.clone, "tag", "v4.0.0-rc.1")
        result = self.run_preflight()
        self.assertEqual(result.returncode, 1)
        self.assertIn("v4.0.0-rc.1", result.stdout)

    def test_a_tag_moved_to_another_commit_is_refused(self):
        (self.clone / "b.txt").write_text("b")
        git(self.clone, "add", "b.txt")
        git(self.clone, "commit", "-q", "-m", "b")
        git(self.clone, "push", "-q", "origin", "main")
        git(self.clone, "tag", "-f", "-a", "-m", "moved", "v3.3.0")
        result = self.run_preflight()
        self.assertEqual(result.returncode, 1)
        self.assertIn("v3.3.0", result.stdout)
        self.assertIn("here", result.stdout)
        self.assertIn("on origin", result.stdout)

    def test_an_uncommitted_change_is_refused_and_named(self):
        (self.clone / "a.txt").write_text("changed\n")
        result = self.run_preflight()
        self.assertEqual(result.returncode, 1)
        self.assertIn("a.txt", result.stdout)
        self.assertIn("uncommitted", result.stdout)

    def test_a_staged_change_is_refused(self):
        (self.clone / "new.txt").write_text("n")
        git(self.clone, "add", "new.txt")
        result = self.run_preflight()
        self.assertEqual(result.returncode, 1)
        self.assertIn("new.txt", result.stdout)

    def test_an_untracked_file_is_refused_and_named(self):
        (self.clone / "scratch.txt").write_text("s")
        result = self.run_preflight()
        self.assertEqual(result.returncode, 1)
        self.assertIn("scratch.txt", result.stdout)
        self.assertIn("untracked", result.stdout)

    def test_an_unpushed_commit_is_refused_with_its_count(self):
        (self.clone / "c.txt").write_text("c")
        git(self.clone, "add", "c.txt")
        git(self.clone, "commit", "-q", "-m", "c")
        result = self.run_preflight()
        self.assertEqual(result.returncode, 1)
        self.assertIn("not on origin/main", result.stdout)
        self.assertIn("1 commit", result.stdout)

    def test_a_clone_behind_origin_is_refused_with_its_count(self):
        self.push_from_seed("two")
        git(self.clone, "fetch", "-q")  # the objects are here: behind, and it can say by how many
        result = self.run_preflight()
        self.assertEqual(result.returncode, 1)
        self.assertIn("behind origin/main by 1 commit", result.stdout)

    def test_a_clone_behind_origin_without_the_objects_says_to_fetch(self):
        tip = self.push_from_seed("two")
        result = self.run_preflight()  # no fetch: the clone has never seen the new commit
        self.assertEqual(result.returncode, 1)
        self.assertIn(tip[:12], result.stdout)
        self.assertIn("git fetch", result.stdout)

    def test_a_diverged_clone_is_refused_for_both_reasons(self):
        self.push_from_seed("two")
        (self.clone / "c.txt").write_text("c")
        git(self.clone, "add", "c.txt")
        git(self.clone, "commit", "-q", "-m", "c")
        git(self.clone, "fetch", "-q")
        result = self.run_preflight()
        self.assertEqual(result.returncode, 1)
        self.assertIn("not on origin/main", result.stdout)
        self.assertIn("behind origin/main", result.stdout)

    def test_an_unreachable_origin_is_refused_not_skipped(self):
        git(self.clone, "remote", "set-url", "origin", str(self.tmp / "nowhere.git"))
        result = self.run_preflight()
        self.assertEqual(result.returncode, 1)
        self.assertIn("cannot reach origin", result.stdout)

    def test_no_origin_is_refused(self):
        git(self.clone, "remote", "remove", "origin")
        result = self.run_preflight()
        self.assertEqual(result.returncode, 1)
        self.assertIn("no remote named origin", result.stdout)

    def test_every_problem_is_reported_not_just_the_first(self):
        git(self.clone, "tag", "-a", "-m", "v4.0.0", "v4.0.0")
        (self.clone / "a.txt").write_text("changed\n")
        (self.clone / "scratch.txt").write_text("s")
        (self.clone / "c.txt").write_text("c")
        git(self.clone, "add", "c.txt")
        git(self.clone, "commit", "-q", "-m", "c")
        result = self.run_preflight()
        self.assertEqual(result.returncode, 1)
        text = result.stdout
        for needle in ("v4.0.0", "a.txt", "scratch.txt", "not on origin/main"):
            self.assertIn(needle, text)
        self.assertGreaterEqual(len(self.problems(result)), 4, text)

    def test_the_base_branch_is_an_option_and_a_missing_one_is_refused(self):
        git(self.seed, "switch", "-q", "-c", "release")
        git(self.seed, "push", "-q", "origin", "release")  # the same commit as main
        self.assertEqual(self.run_preflight("--base", "release").returncode, 0)
        result = self.run_preflight("--base", "nonesuch")
        self.assertEqual(result.returncode, 1)
        self.assertIn("origin has no branch nonesuch", result.stdout)

    # --- the contract: read-only, logged ------------------------------------------------------------

    def test_it_runs_no_git_command_that_changes_anything(self):
        shim_dir = self.tmp / "shim"
        shim_dir.mkdir()
        argv_log = self.tmp / "argv.log"
        real_git = shutil.which("git")
        shim = shim_dir / "git"
        shim.write_text(f"#!/usr/bin/env bash\nprintf '%s\\n' \"$*\" >> '{argv_log}'\nexec '{real_git}' \"$@\"\n")
        shim.chmod(shim.stat().st_mode | stat.S_IXUSR)
        before = git(self.clone, "for-each-ref").stdout
        self.push_from_seed("two")
        result = self.run_preflight(env={"PATH": f"{shim_dir}:{os.environ['PATH']}"})
        self.assertEqual(result.returncode, 1)
        calls = argv_log.read_text().splitlines()
        self.assertTrue(calls, "the shim saw no git call: the sensor is blind")
        allowed = {"rev-parse", "remote", "status", "for-each-ref", "ls-remote", "cat-file", "rev-list"}
        for call in calls:
            words = call.split()
            while words and words[0] == "-c":
                words = words[2:]
            self.assertIn(words[0], allowed, f"a git call outside the read-only list: {call}")
            if words[0] == "remote":
                self.assertEqual(words[1], "get-url", f"a changing remote call: {call}")
        self.assertTrue(any("ls-remote" in c for c in calls))
        after = git(self.clone, "for-each-ref").stdout
        self.assertEqual(before, after, "the clone's refs changed")

    def test_a_run_is_logged_as_one_json_line(self):
        git(self.clone, "tag", "-a", "-m", "v4.0.0", "v4.0.0")
        self.run_preflight()
        self.run_preflight()
        lines = self.log.read_text().splitlines()
        self.assertEqual(len(lines), 2)
        record = json.loads(lines[0])
        for key in ("ts", "user", "cwd", "head", "base", "result", "problems"):
            self.assertIn(key, record)
        self.assertEqual(record["result"], "refused")
        self.assertTrue(any("v4.0.0" in p for p in record["problems"]))

    def test_a_log_that_cannot_be_written_aborts_the_run(self):
        result = self.run_preflight(env={"RELEASE_PREFLIGHT_LOG": str(self.tmp / "no" / "such" / "dir" / "x.jsonl")})
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("cannot write the log", result.stdout + result.stderr)


class RecipeAndDocsTest(unittest.TestCase):
    def test_package_dev_runs_the_preflight_first_and_has_no_inline_status_check(self):
        text = (REPO / "justfile").read_text()
        start = text.index("\npackage-dev ")
        body = text[start:text.index("\n\n", start)]
        after = body[body.index("set -euo pipefail"):].splitlines()[1:]
        first_command = next(l.strip() for l in after if l.strip() and not l.strip().startswith("#"))
        self.assertEqual(first_command, "tools/release_preflight.py")
        self.assertNotIn("git status --porcelain", body)
        self.assertIn("\nrelease-preflight", text)

    def test_the_agents_md_release_steps_name_the_preflight_and_the_tag_gate(self):
        text = (REPO / "AGENTS.md").read_text()
        self.assertIn("just release-preflight", text)
        self.assertIn("gh release create", text)

    def test_the_script_is_executable_and_in_the_tree(self):
        self.assertTrue(os.access(SCRIPT, os.X_OK))


if __name__ == "__main__":
    unittest.main()
