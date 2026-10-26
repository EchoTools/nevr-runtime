"""tools/remove_worktree.py on a fixture repository: a main checkout with two linked worktrees, one initialised
submodule (the stand-in for extern/luau) and one submodule that worktrees hold as a plain copy (the stand-in for
extern/breakpad, which `just worktree-setup` copies without a `.git`).

Every git call goes through tools/tests/git_fixture.py (the GIT_* variables removed), and each run of the tool is
wrapped in real_repo_unchanged: the checkout these tests run from must keep byte-identical submodule config and
worktree list.
"""
from __future__ import annotations

import json
import os
import pathlib
import shutil
import stat
import subprocess
import sys
import unittest

REPO = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "tools"))
sys.path.insert(0, str(REPO / "tools/tests"))
import work_root  # noqa: E402
from git_fixture import clean_env, git, real_repo_unchanged  # noqa: E402

SCRIPT = REPO / "tools/remove_worktree.py"
SHARED = ["config", "--get-regexp", r"^submodule\."]


class FixtureCase(unittest.TestCase):
    def setUp(self):
        scratch = work_root.make_test_dir("remove-worktree-test-")
        self.addCleanup(scratch.cleanup)
        self.tmp = pathlib.Path(scratch.name)
        for name in ("luau", "breakpad"):
            up = self.tmp / f"up-{name}"
            git(self.tmp, "init", "-q", "-b", "master", str(up))
            (up / "file.txt").write_text(f"{name} 1\n")
            git(up, "add", "file.txt")
            git(up, "commit", "-q", "-m", f"{name} 1", "--no-gpg-sign")
        self.main = self.tmp / "main"
        git(self.tmp, "init", "-q", "-b", "main", str(self.main))
        git(self.main, "submodule", "add", "-q", str(self.tmp / "up-luau"), "extern/luau")
        git(self.main, "submodule", "add", "-q", str(self.tmp / "up-breakpad"), "extern/breakpad")
        (self.main / ".gitignore").write_text("build/\n")
        (self.main / "README").write_text("main\n")
        git(self.main, "add", "README", ".gitignore")
        git(self.main, "commit", "-q", "-m", "super", "--no-gpg-sign")
        self.log = self.tmp / "remove-worktree.jsonl"

    def add_worktree(self, name, *, luau=True, copy=True, lock=None):
        wt = self.tmp / name
        git(self.main, "worktree", "add", "-q", "--no-track", "-b", name, str(wt), "main")
        if luau:
            git(wt, "submodule", "update", "--init", "extern/luau")
        if copy:
            shutil.copytree(self.main / "extern/breakpad", wt / "extern/breakpad", dirs_exist_ok=True,
                            ignore=shutil.ignore_patterns(".git"))
        if lock:
            git(self.main, "worktree", "lock", "--reason", lock, str(wt))
        return wt

    def shared_config(self):
        return git(self.main, *SHARED, check=False).stdout

    def admin(self, name):
        return self.main / ".git/worktrees" / name

    def run_tool(self, *args, cwd=None, path_prefix=None):
        env = clean_env(REMOVE_WORKTREE_LOG=str(self.log))
        if path_prefix:
            env["PATH"] = f"{path_prefix}{os.pathsep}{env['PATH']}"
        with real_repo_unchanged(self):
            return subprocess.run([sys.executable, "-I", str(SCRIPT), *args], cwd=cwd or self.tmp, env=env,
                                  capture_output=True, text=True, timeout=120)

    def records(self):
        return [json.loads(line) for line in self.log.read_text().splitlines()]


class RemoveWorktreeTest(FixtureCase):
    def test_premise_deinit_all_unregisters_the_submodule_for_every_worktree(self):
        # The reason the tool exists: the procedure it replaces. If git stops doing this, say so here.
        wa, wb = self.add_worktree("wa"), self.add_worktree("wb")
        shutil.rmtree(wa / "extern/breakpad")
        (wa / "extern/breakpad").mkdir()
        self.assertIn("submodule.extern/luau.url", self.shared_config())
        git(wa, "submodule", "deinit", "--all")
        self.assertNotIn("submodule.extern/luau", self.shared_config())
        self.assertEqual(git(wb, "config", "--get-regexp", r"^submodule\.extern/luau", check=False).returncode, 1,
                         "wb lost the registration although only wa was deinitialised")

    def test_premise_git_refuses_to_remove_a_worktree_that_has_submodule_gitdirs(self):
        wb = self.add_worktree("wb")
        r = git(self.main, "worktree", "remove", str(wb), check=False)
        self.assertNotEqual(r.returncode, 0)
        self.assertIn("containing submodules", r.stderr)

    def test_removal_leaves_the_other_worktree_resolving_the_submodule_and_the_shared_config_unchanged(self):
        wa, wb = self.add_worktree("wa"), self.add_worktree("wb", lock="fixture lock")
        before = self.shared_config()
        self.assertIn("submodule.extern/luau.active true", before)
        result = self.run_tool(str(wb))
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertFalse(wb.exists())
        self.assertFalse(self.admin("wb").exists())
        self.assertEqual(self.shared_config(), before)
        # wa still resolves its own submodule: registered, checked out, repository intact.
        self.assertRegex(git(wa, "submodule", "status", "extern/luau").stdout, r"^ [0-9a-f]{40} extern/luau")
        self.assertEqual(git(wa / "extern/luau", "log", "--format=%s", "-1").stdout.strip(), "luau 1")
        self.assertTrue((self.admin("wa") / "modules/extern/luau").is_dir())
        self.assertEqual(git(wa, "status", "--short").stdout, "")

    def test_it_never_runs_deinit_or_force(self):
        wb = self.add_worktree("wb")
        bin_dir = self.tmp / "bin"
        bin_dir.mkdir()
        record = self.tmp / "git-calls.txt"
        real_git = shutil.which("git")
        wrapper = bin_dir / "git"
        wrapper.write_text(f'#!/bin/sh\nprintf \'%s\\n\' "$*" >> {record}\nexec {real_git} "$@"\n')
        wrapper.chmod(0o755)
        result = self.run_tool(str(wb), path_prefix=str(bin_dir))
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        calls = record.read_text().splitlines()
        self.assertTrue(any(c.startswith("worktree remove ") for c in calls), calls)
        for c in calls:
            self.assertNotIn("deinit", c)
            self.assertNotRegex(c, r"(^| )(--force|-f)( |$)")
            self.assertNotRegex(c, r"submodule (init|update)")

    def test_a_plain_copy_that_differs_from_the_main_checkout_is_refused_and_nothing_changes(self):
        wb = self.add_worktree("wb")
        (wb / "extern/breakpad/file.txt").write_text("edited in the worktree\n")
        before = self.shared_config()
        result = self.run_tool(str(wb))
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn("extern/breakpad: a plain copy that differs from the main checkout's (file.txt: contents differ)",
                      result.stderr)
        self.assertEqual((wb / "extern/breakpad/file.txt").read_text(), "edited in the worktree\n")
        self.assertTrue((wb / "extern/luau/.git").is_file())
        self.assertTrue((self.admin("wb") / "modules").is_dir())
        self.assertEqual(self.shared_config(), before)

    def test_a_plain_copy_with_an_extra_file_is_refused(self):
        wb = self.add_worktree("wb")
        (wb / "extern/breakpad/notes.txt").write_text("mine\n")
        result = self.run_tool(str(wb))
        self.assertEqual(result.returncode, 1)
        self.assertIn("notes.txt: only in the worktree", result.stderr)
        self.assertTrue(wb.exists())

    def test_a_plain_copy_the_main_checkout_cannot_confirm_is_refused(self):
        wb = self.add_worktree("wb")
        shutil.rmtree(self.main / "extern/breakpad")
        result = self.run_tool(str(wb))
        self.assertEqual(result.returncode, 1)
        self.assertIn("the main checkout has no extern/breakpad to compare it with", result.stderr)
        self.assertTrue(wb.exists())

    def test_a_commit_in_the_submodule_that_no_remote_holds_is_refused(self):
        wb = self.add_worktree("wb")
        sub = wb / "extern/luau"
        pinned = git(sub, "rev-parse", "HEAD").stdout.strip()
        git(sub, "checkout", "-q", "-b", "side")
        (sub / "work.txt").write_text("unpushed\n")
        git(sub, "add", "work.txt")
        git(sub, "commit", "-q", "-m", "unpushed work", "--no-gpg-sign")
        git(sub, "checkout", "-q", "--detach", pinned)
        self.assertEqual(git(wb, "status", "--short").stdout, "", "the superproject must look clean for this test")
        result = self.run_tool(str(wb))
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn("extern/luau: 1 commit(s) in its repository are on no remote", result.stderr)
        self.assertEqual(git(sub, "log", "--format=%s", "side", "-1").stdout.strip(), "unpushed work")
        self.assertTrue((sub / ".git").is_file())

    def test_uncommitted_work_inside_the_submodule_is_refused(self):
        wb = self.add_worktree("wb")
        (wb / "extern/luau/scratch.txt").write_text("untracked\n")
        result = self.run_tool(str(wb))
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn("extern/luau: uncommitted or untracked work inside the submodule", result.stderr)
        self.assertTrue(wb.exists())

    def test_a_submodule_repository_that_is_not_this_worktrees_own_is_refused(self):
        wb = self.add_worktree("wb")
        other = self.admin("elsewhere/modules/extern/luau")
        shutil.copytree(self.admin("wb") / "modules/extern/luau", other)
        (wb / "extern/luau/.git").write_text(f"gitdir: {other}\n")
        result = self.run_tool(str(wb))
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertRegex(result.stderr, r"extern/luau: its repository is .*not under this worktree's own")
        self.assertTrue(wb.exists())

    def test_a_process_with_its_cwd_inside_is_refused(self):
        wb = self.add_worktree("wb")
        proc = subprocess.Popen(["sleep", "30"], cwd=wb)
        self.addCleanup(lambda: (proc.kill(), proc.wait()))
        result = self.run_tool(str(wb))
        self.assertEqual(result.returncode, 1)
        self.assertRegex(result.stderr, rf"process cwd inside: {proc.pid} sleep")
        self.assertTrue(wb.exists())

    def test_untracked_work_is_refused(self):
        wb = self.add_worktree("wb")
        (wb / "notes.txt").write_text("mine\n")
        result = self.run_tool(str(wb))
        self.assertEqual(result.returncode, 1)
        self.assertIn("uncommitted or untracked work: ?? notes.txt", result.stderr)

    def test_an_ignored_file_that_is_not_build_output_is_refused(self):
        wb = self.add_worktree("wb")
        (wb / ".gitignore").write_text("build/\n*.log\n")
        git(wb, "add", ".gitignore")
        git(wb, "commit", "-q", "-m", "ignore logs", "--no-gpg-sign")
        (wb / "run.log").write_text("evidence\n")
        result = self.run_tool(str(wb))
        self.assertEqual(result.returncode, 1)
        self.assertIn("ignored files that are not build output", result.stderr)
        self.assertIn("run.log", result.stderr)

    def test_build_output_goes_with_the_worktree(self):
        wb = self.add_worktree("wb")
        (wb / "build").mkdir()
        (wb / "build/obj.o").write_text("x")
        result = self.run_tool(str(wb))
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertFalse(wb.exists())

    def test_the_main_checkout_and_a_subdirectory_are_refused(self):
        wb = self.add_worktree("wb")
        for target in (self.main, wb / "extern"):
            result = self.run_tool(str(target))
            self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
            self.assertIn("not the root of a linked worktree", result.stderr)
        self.assertTrue(self.main.exists() and wb.exists())

    def test_a_dry_run_makes_every_proof_and_changes_nothing(self):
        wb = self.add_worktree("wb", lock="fixture lock")
        before = self.shared_config()
        result = self.run_tool("--dry-run", str(wb))
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("would run git worktree remove (no --force)", result.stdout)
        self.assertTrue((wb / "extern/luau/.git").is_file())
        self.assertTrue((self.admin("wb") / "modules").is_dir())
        self.assertIn("locked", git(self.main, "worktree", "list", "--porcelain").stdout)
        self.assertEqual(self.shared_config(), before)
        dirty = self.add_worktree("wc")
        (dirty / "extern/breakpad/file.txt").write_text("edited\n")
        self.assertEqual(self.run_tool("--dry-run", str(dirty)).returncode, 1)

    def test_a_worktree_without_submodules_checked_out_is_removed(self):
        wb = self.add_worktree("wb", luau=False, copy=False)
        result = self.run_tool(str(wb))
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertFalse(wb.exists())

    def test_every_decision_is_logged(self):
        wb = self.add_worktree("wb")
        wc = self.add_worktree("wc")
        (wc / "notes.txt").write_text("mine\n")
        self.assertEqual(self.run_tool(str(wb)).returncode, 0)
        self.assertEqual(self.run_tool(str(wc)).returncode, 1)
        actions = [r["action"] for r in self.records()]
        for a in ("start", "plan", "submodule-gitfile-removed", "modules-moved-aside", "worktree-remove", "end", "refused"):
            self.assertIn(a, actions)
        refused = [r for r in self.records() if r["action"] == "refused"][0]
        self.assertTrue(any("notes.txt" in reason for reason in refused["reasons"]))

    @unittest.skipIf(os.geteuid() == 0, "root can delete from a read-only directory")
    def test_a_failure_after_the_first_change_exits_2_and_says_what_was_done(self):
        wb = self.add_worktree("wb", lock="fixture lock")
        locked = wb / "build/locked"
        locked.mkdir(parents=True)
        (locked / "f").write_text("x")
        locked.chmod(stat.S_IRUSR | stat.S_IXUSR)
        self.addCleanup(lambda: locked.chmod(stat.S_IRWXU))
        before = self.shared_config()
        result = self.run_tool(str(wb))
        self.assertEqual(result.returncode, 2, result.stdout + result.stderr)
        self.assertIn("remove-worktree: FAILED", result.stderr)
        self.assertIn("deleted the .git file of extern/luau", result.stderr)
        self.assertIn("git has already unregistered the worktree", result.stderr)
        self.assertEqual(self.shared_config(), before)
        # Git deletes the administrative directory even though the worktree directory would not go.
        self.assertFalse(self.admin("wb").exists())
        self.assertTrue(wb.exists())
        failed = [r for r in self.records() if r["action"] == "failed"]
        self.assertEqual(len(failed), 1)
        self.assertIn("unlocked", failed[0]["done"])
        self.assertTrue(failed[0]["unregistered"])

    def test_a_removal_that_changes_the_shared_config_is_reported_loudly(self):
        # A git that unregistered the submodule while removing (what deinit does) must not pass unnoticed.
        wb = self.add_worktree("wb")
        bin_dir = self.tmp / "bin"
        bin_dir.mkdir()
        real_git = shutil.which("git")
        wrapper = bin_dir / "git"
        wrapper.write_text('#!/bin/sh\ncase "$*" in *"worktree remove"*) %s -C %s config --unset-all submodule.extern/luau.url;; esac\n'
                           'exec %s "$@"\n' % (real_git, self.main, real_git))
        wrapper.chmod(0o755)
        result = self.run_tool(str(wb), path_prefix=str(bin_dir))
        self.assertEqual(result.returncode, 2, result.stdout + result.stderr)
        self.assertIn("the shared submodule config changed", result.stderr)
        self.assertIn("shared-config-changed", [r["action"] for r in self.records()])


if __name__ == "__main__":
    unittest.main()
