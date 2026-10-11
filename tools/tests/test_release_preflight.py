"""tools/release_preflight.py (`just release-preflight`): a release is only started from a pristine clone.

Real git in scratch repositories: a bare "origin" and a clone of it. Each refusal has its own test; the
pristine clone passes; and the check is read-only: no fetch, pull, push, tag, checkout or reset is ever run.
"""
from __future__ import annotations

import json
import os
import pathlib
import re
import shutil
import stat
import subprocess
import sys
import tempfile
import unittest

REPO = pathlib.Path(__file__).resolve().parents[2]
SCRIPT = REPO / "tools" / "release_preflight.py"


def git(cwd, *args, env=None):
    return subprocess.run(["git", "-c", "user.name=t", "-c", "user.email=t@example.invalid",
                           "-c", "commit.gpgsign=false", "-c", "tag.gpgsign=false", *args],
                          cwd=cwd, check=True, capture_output=True, text=True, env=env)


def scratch_expect(origin) -> str:
    """The --expect-origin pattern that accepts the scratch bare repository's path."""
    return "^" + re.escape(str(origin)) + "$"


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

    def run_preflight(self, *extra, env=None, cwd=None, expect=True):
        e = dict(os.environ, RELEASE_PREFLIGHT_LOG=str(self.log))
        if env:
            e.update(env)
        args = list(extra)
        if expect and "--expect-origin" not in args:
            args += ["--expect-origin", scratch_expect(self.origin)]
        return subprocess.run([str(SCRIPT), *args], cwd=cwd or self.clone, env=e, capture_output=True, text=True)

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
        git(self.clone, "tag", "v5.0.0-beta.1")
        result = self.run_preflight()
        self.assertEqual(result.returncode, 1)
        self.assertIn("v5.0.0-beta.1", result.stdout)

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
        result = self.run_preflight("--expect-origin", scratch_expect(self.tmp / "nowhere.git"))
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

    def test_an_origin_that_is_not_the_project_is_refused_and_named(self):
        result = self.run_preflight(expect=False)  # the default pattern: the project's GitHub URL
        self.assertEqual(result.returncode, 1)
        self.assertIn(str(self.origin), result.stdout)
        self.assertIn("not the project's repository", result.stdout)

    def test_both_github_url_forms_of_the_project_are_accepted(self):
        for url in ("git@github.com:EchoTools/nevr-runtime.git", "https://github.com/EchoTools/nevr-runtime.git",
                    "https://github.com/EchoTools/nevr-runtime", "ssh://git@github.com/EchoTools/nevr-runtime.git"):
            with self.subTest(url=url):
                git(self.clone, "config", "remote.origin.url", url)
                # url.<local bare repository>.insteadOf lets git reach the scratch origin instead of GitHub
                git(self.clone, "config", f"url.{self.origin}.insteadOf", url)
                result = self.run_preflight(expect=False)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                git(self.clone, "config", "--unset", f"url.{self.origin}.insteadOf")

    def test_another_github_repository_is_refused(self):
        for url in ("git@github.com:someone/nevr-runtime.git", "https://github.com/EchoTools/nevr-runtime-plugins.git",
                    "https://example.com/EchoTools/nevr-runtime.git"):
            with self.subTest(url=url):
                git(self.clone, "config", "remote.origin.url", url)
                git(self.clone, "config", f"url.{self.origin}.insteadOf", url)
                result = self.run_preflight(expect=False)
                self.assertEqual(result.returncode, 1)
                self.assertIn("not the project's repository", result.stdout)
                git(self.clone, "config", "--unset", f"url.{self.origin}.insteadOf")

    def test_a_missing_base_branch_is_refused(self):
        result = self.run_preflight("--base", "nonesuch")
        self.assertEqual(result.returncode, 1)
        self.assertIn("origin has no branch nonesuch", result.stdout)

    def test_a_failing_git_command_is_a_problem_not_a_clean_tree(self):
        shim_dir = self.tmp / "failshim"
        shim_dir.mkdir()
        real_git = shutil.which("git")
        for sub, label in (("status", "git status"), ("for-each-ref", "git for-each-ref refs/tags"), ("rev-parse HEAD", "git rev-parse HEAD")):
            with self.subTest(command=sub):
                shim = shim_dir / "git"
                first, _, second = sub.partition(" ")
                cond = f'[ "$1" = "{first}" ]' + (f' && [ "$2" = "{second}" ]' if second else "")
                shim.write_text(f"#!/usr/bin/env bash\nif {cond}; then echo 'fatal: simulated failure' >&2; exit 128; fi\n"
                                f"exec '{real_git}' \"$@\"\n")
                shim.chmod(shim.stat().st_mode | stat.S_IXUSR)
                result = self.run_preflight(env={"PATH": f"{shim_dir}:{os.environ['PATH']}"})
                self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
                self.assertIn(f"{label} failed (exit 128)", result.stdout)
                self.assertNotIn("release-preflight: OK", result.stdout)

    def shimmed(self, script_body):
        """A git wrapper on PATH: `script_body` (bash) may handle a call and exit; otherwise real git runs."""
        shim_dir = self.tmp / f"shim{len(list(self.tmp.glob('shim*')))}"
        shim_dir.mkdir()
        shim = shim_dir / "git"
        shim.write_text(f"#!/usr/bin/env bash\n{script_body}\nexec '{shutil.which('git')}' \"$@\"\n")
        shim.chmod(shim.stat().st_mode | stat.S_IXUSR)
        return {"PATH": f"{shim_dir}:{os.environ['PATH']}"}

    def test_a_failing_object_lookup_is_a_problem_but_an_absent_object_still_says_fetch(self):
        self.push_from_seed("two")  # origin's tip is not in this clone
        failing = self.shimmed('if [ "$1" = "rev-parse" ] && [ "$2" = "--verify" ]; then '
                               'echo "fatal: simulated" >&2; exit 128; fi')
        result = self.run_preflight(env=failing)
        self.assertEqual(result.returncode, 1)
        self.assertIn("git rev-parse --verify", result.stdout)
        self.assertIn("failed (exit 128)", result.stdout)
        self.assertNotIn("git fetch, then re-run", result.stdout)
        absent = self.run_preflight()  # real git: --verify --quiet exits 1, silently, for an absent object
        self.assertIn("git fetch, then re-run", absent.stdout)
        self.assertNotIn("failed (exit", absent.stdout)

    def test_a_rev_list_that_fails_or_prints_garbage_is_a_problem_never_zero_commits(self):
        self.push_from_seed("two")
        git(self.clone, "fetch", "-q")  # the objects are here: the counting path runs
        for name, body in (("exit 128", 'if [ "$1" = "rev-list" ]; then echo "fatal: simulated" >&2; exit 128; fi'),
                           ("garbage", 'if [ "$1" = "rev-list" ]; then echo "not-a-number"; exit 0; fi')):
            with self.subTest(rev_list=name):
                result = self.run_preflight(env=self.shimmed(body))
                self.assertEqual(result.returncode, 1, result.stdout)
                self.assertIn("git rev-list", result.stdout)
                self.assertNotIn("release-preflight: OK", result.stdout)

    def test_the_log_record_names_the_origin_pattern_in_force(self):
        self.run_preflight()
        record = json.loads(self.log.read_text().splitlines()[0])
        self.assertEqual(record["expect_origin"], scratch_expect(self.origin))
        self.run_preflight("--expect-origin", ".*")
        self.assertEqual(json.loads(self.log.read_text().splitlines()[1])["expect_origin"], ".*")

    def test_a_tracked_change_alone_is_refused(self):
        (self.clone / "a.txt").write_text("changed\n")
        result = self.run_preflight()
        self.assertEqual(result.returncode, 1)
        self.assertEqual(len([p for p in self.problems(result) if "a.txt" in p]), 1)
        self.assertEqual(len(self.problems(result)), 1, result.stdout)

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
        allowed = {"rev-parse", "config", "status", "for-each-ref", "ls-remote", "rev-list"}
        for call in calls:
            words = call.split()
            while words and words[0] == "-c":
                words = words[2:]
            self.assertIn(words[0], allowed, f"a git call outside the read-only list: {call}")
            if words[0] == "config":
                self.assertEqual(words[1:], ["--get", "remote.origin.url"], f"a changing config call: {call}")
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


class MissingGitTest(unittest.TestCase):
    """git missing from PATH is a refusal with a log record, not a traceback and exit 1 with nothing written."""

    def test_an_empty_path_is_refused_loudly_and_logged(self):
        with tempfile.TemporaryDirectory(prefix="no-git-", dir="/var/tmp") as tmp:
            log = pathlib.Path(tmp) / "preflight.jsonl"
            result = subprocess.run([sys.executable, str(SCRIPT)], cwd=tmp, capture_output=True, text=True,
                                    env={"PATH": "", "RELEASE_PREFLIGHT_LOG": str(log)})
            self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
            self.assertNotIn("Traceback", result.stderr)
            self.assertIn("release-preflight: PROBLEM: git rev-parse --show-toplevel failed (exit 127)", result.stdout)
            self.assertIn("git is not installed or not on PATH", result.stdout)
            record = json.loads(log.read_text().splitlines()[0])
            self.assertEqual(record["result"], "refused")
            self.assertTrue(any("not installed" in p for p in record["problems"]))


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
