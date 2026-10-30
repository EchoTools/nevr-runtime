"""tools/remove_worktree.py on a fixture repository: a main checkout with two linked worktrees, one initialised
submodule (the stand-in for extern/luau) and one submodule that worktrees hold as a plain copy (the stand-in for
extern/breakpad, which `just worktree-setup` copies without a `.git`).

Every git call goes through tools/tests/gitenv.py (the GIT_* variables removed), and each run of the tool is
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
import unittest.mock

REPO = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "tools"))
sys.path.insert(0, str(REPO / "tools/tests"))
import work_root  # noqa: E402
import gitenv  # noqa: E402
import real_repo_guard  # noqa: E402
from real_repo_guard import SUBMODULE_ENV, real_repo_unchanged  # noqa: E402

SCRIPT = REPO / "tools/remove_worktree.py"


def git(cwd, *args, check=True):
    return gitenv.git(cwd, *args, check=check, env=SUBMODULE_ENV)


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
        (self.main / ".gitignore").write_text("build/\n.mcp.json\n.claude/settings.local.json\nconfig/public-defaults.env\n.pytest_cache/\n")
        (self.main / "README").write_text("main\n")
        for rel in (".claude/skills.md", "config/public-defaults.env.example"):  # tracked, beside the ignored copies
            (self.main / rel).parent.mkdir(parents=True, exist_ok=True)
            (self.main / rel).write_text("tracked\n")
        git(self.main, "add", "README", ".gitignore", ".claude", "config")
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

    def run_tool(self, *args, cwd=None, path_prefix=None, **extra_env):
        env = gitenv.clean_env()
        env.update(REMOVE_WORKTREE_LOG=str(self.log), **extra_env)
        if path_prefix:
            env["PATH"] = f"{path_prefix}{os.pathsep}{env['PATH']}"
        with real_repo_unchanged(self):
            return subprocess.run([sys.executable, "-I", str(SCRIPT), *args], cwd=cwd or self.tmp, env=env,
                                  capture_output=True, text=True, timeout=120)

    def records(self):
        return [json.loads(line) for line in self.log.read_text().splitlines()]

    def ignore(self, *patterns):
        # .git/info/exclude is shared by every worktree of the fixture, so one line covers all of them.
        with open(self.main / ".git/info/exclude", "a", encoding="utf-8") as f:
            f.write("".join(p + "\n" for p in patterns))

    def commit_in(self, wb, rel, text):
        (wb / rel).parent.mkdir(parents=True, exist_ok=True)
        (wb / rel).write_text(text)
        git(wb, "add", rel)
        git(wb, "commit", "-q", "-m", f"add {rel}", "--no-gpg-sign")

    def cachedir_tag(self, directory, signature="Signature: 8a477f597d28d172789f06886806bc55"):
        directory.mkdir(parents=True, exist_ok=True)
        (directory / "CACHEDIR.TAG").write_text(signature + "\n# This file is a cache directory tag.\n")


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

    def test_a_commit_on_a_detached_head_in_the_submodule_is_counted(self):
        wb = self.add_worktree("wb")
        sub = wb / "extern/luau"
        (sub / "work.txt").write_text("unpushed\n")
        git(sub, "add", "work.txt")
        git(sub, "commit", "-q", "-m", "detached work", "--no-gpg-sign")
        result = self.run_tool(str(wb))
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn("extern/luau: 1 commit(s) in its repository are on no remote", result.stderr)
        self.assertTrue(wb.exists())

    def test_a_stray_git_dir_in_the_environment_does_not_redirect_the_tool(self):
        wb = self.add_worktree("wb")
        result = self.run_tool(str(wb), GIT_DIR=str(self.main / ".git"), GIT_WORK_TREE=str(self.main))
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertFalse(wb.exists())

    def test_a_removal_git_refuses_puts_the_lock_the_modules_and_the_gitfile_back(self):
        wb = self.add_worktree("wb", lock="fixture lock")
        bin_dir = self.tmp / "bin"
        bin_dir.mkdir()
        wrapper = bin_dir / "git"
        wrapper.write_text('#!/bin/sh\ncase "$*" in *"worktree remove"*) echo "fatal: refused by the test" >&2; exit 1;; esac\n'
                           'exec %s "$@"\n' % shutil.which("git"))
        wrapper.chmod(0o755)
        before = self.shared_config()
        result = self.run_tool(str(wb), path_prefix=str(bin_dir))
        self.assertEqual(result.returncode, 2, result.stdout + result.stderr)
        self.assertIn("put back: the modules directory, .git file of extern/luau, the lock", result.stderr)
        self.assertTrue((self.admin("wb") / "modules/extern/luau").is_dir())
        self.assertTrue((wb / "extern/luau/.git").is_file())
        self.assertIn("locked fixture lock", git(self.main, "worktree", "list", "--porcelain").stdout)
        self.assertRegex(git(wb, "submodule", "status", "extern/luau").stdout, r"^ [0-9a-f]{40} extern/luau")
        self.assertEqual(self.shared_config(), before)
        # and the tool still removes it afterwards
        self.assertEqual(self.run_tool(str(wb)).returncode, 0)

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

    def test_a_cargo_target_with_a_cachedir_tag_is_build_output(self):
        # installer/target/ is cargo's build directory; the CACHEDIR.TAG in it marks it as a cache.
        self.ignore("installer/target/")
        wb = self.add_worktree("wb")
        self.commit_in(wb, "installer/Cargo.toml", "[package]\n")
        self.cachedir_tag(wb / "installer/target")
        (wb / "installer/target/debug").mkdir()
        (wb / "installer/target/debug/build.o").write_text("x")
        result = self.run_tool(str(wb))
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertFalse(wb.exists())

    def test_a_directory_holding_only_pycache_is_build_output(self):
        # git prints tools/perf/ as one entry whose only content is __pycache__/: that is build output.
        self.ignore("__pycache__/")
        wb = self.add_worktree("wb")
        self.commit_in(wb, "tools/tracked.py", "x = 1\n")
        (wb / "tools/perf/__pycache__").mkdir(parents=True)
        (wb / "tools/perf/__pycache__/x.pyc").write_text("x")
        result = self.run_tool(str(wb))
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertFalse(wb.exists())

    def test_a_directory_whose_only_content_is_a_tagged_directory_is_build_output(self):
        # git prints installer/ as one entry; the tagged target/ inside it is build output, so installer/ is too.
        self.ignore("installer/target/")
        wb = self.add_worktree("wb")
        self.cachedir_tag(wb / "installer/target")
        (wb / "installer/target/debug").mkdir()
        (wb / "installer/target/debug/build.o").write_text("x")
        result = self.run_tool(str(wb))
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertFalse(wb.exists())

    def test_a_cargo_target_without_a_cachedir_tag_is_refused(self):
        self.ignore("installer/target/")
        wb = self.add_worktree("wb")
        self.commit_in(wb, "installer/Cargo.toml", "[package]\n")
        (wb / "installer/target/debug").mkdir(parents=True)
        (wb / "installer/target/debug/build.o").write_text("x")
        result = self.run_tool(str(wb))
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn("ignored files that are not build output", result.stderr)
        self.assertIn("installer/target", result.stderr)
        self.assertTrue(wb.exists())

    def test_a_cachedir_tag_with_another_signature_is_not_build_output(self):
        self.ignore("installer/target/")
        wb = self.add_worktree("wb")
        self.commit_in(wb, "installer/Cargo.toml", "[package]\n")
        self.cachedir_tag(wb / "installer/target", signature="Signature: 00000000000000000000000000000000")
        (wb / "installer/target/debug").mkdir()
        (wb / "installer/target/debug/build.o").write_text("x")
        result = self.run_tool(str(wb))
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn("installer/target", result.stderr)
        self.assertTrue(wb.exists())

    def test_a_run_log_beside_pycache_in_a_collapsed_directory_is_refused(self):
        self.ignore("__pycache__/", "*.log")
        wb = self.add_worktree("wb")
        self.commit_in(wb, "tools/tracked.py", "x = 1\n")
        (wb / "tools/perf/__pycache__").mkdir(parents=True)
        (wb / "tools/perf/__pycache__/x.pyc").write_text("x")
        (wb / "tools/perf/run.log").write_text("evidence\n")
        result = self.run_tool(str(wb))
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn("tools/perf/run.log", result.stderr)
        self.assertTrue(wb.exists())

    def test_build_output_is_accepted_by_name_and_not_listed_file_by_file(self):
        # A build directory can hold thousands of files: it is judged by its name and never expanded.
        wb = self.add_worktree("wb")
        (wb / "build").mkdir()
        (wb / "build/obj.o").write_text("x")
        bin_dir = self.tmp / "bin"
        bin_dir.mkdir()
        record = self.tmp / "ls-files-calls.txt"
        wrapper = bin_dir / "git"
        wrapper.write_text(
            "#!/bin/sh\n"
            f'case "$*" in *"--others --ignored"*) printf \'%s\\n\' "$*" >> {record};; esac\n'
            f'exec {shutil.which("git")} "$@"\n')
        wrapper.chmod(0o755)
        result = self.run_tool(str(wb), path_prefix=str(bin_dir))
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        calls = record.read_text() if record.exists() else ""
        self.assertNotIn("build/", calls)

    def test_a_collapsed_directory_under_which_git_lists_no_file_is_refused(self):
        # The tool cannot see what a collapsed directory holds when git lists nothing under it: refuse, never accept.
        self.ignore("__pycache__/")
        wb = self.add_worktree("wb")
        self.commit_in(wb, "tools/tracked.py", "x = 1\n")
        (wb / "tools/perf/__pycache__").mkdir(parents=True)
        (wb / "tools/perf/__pycache__/x.pyc").write_text("x")
        bin_dir = self.tmp / "bin"
        bin_dir.mkdir()
        wrapper = bin_dir / "git"
        wrapper.write_text('#!/bin/sh\ncase "$*" in *"--others --ignored"*) exit 0;; esac\n'
                           'exec %s "$@"\n' % shutil.which("git"))
        wrapper.chmod(0o755)
        result = self.run_tool(str(wb), path_prefix=str(bin_dir))
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn("tools/perf/", result.stderr)
        self.assertTrue(wb.exists())

    def test_a_run_log_beside_a_tagged_directory_is_refused(self):
        self.ignore("installer/target/", "*.log")
        wb = self.add_worktree("wb")
        self.commit_in(wb, "installer/Cargo.toml", "[package]\n")
        self.cachedir_tag(wb / "installer/target")
        (wb / "installer/run.log").write_text("evidence\n")
        result = self.run_tool(str(wb))
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn("installer/run.log", result.stderr)
        self.assertTrue(wb.exists())

    def test_the_files_a_new_worktree_is_given_go_with_it(self):
        # What the post-checkout hook and `just worktree-setup` copy in, and the pytest cache.
        wb = self.add_worktree("wb")
        for rel in (".mcp.json", ".claude/settings.local.json", "config/public-defaults.env", ".pytest_cache/v/cache/x"):
            (wb / rel).parent.mkdir(parents=True, exist_ok=True)
            (wb / rel).write_text("copied\n")
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
        for a in ("start", "plan", "submodule-gitfile-removed", "modules-moved-aside", "git-worktree-remove", "end", "refused"):
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


class RealRepoGuardTest(unittest.TestCase):
    def test_the_guard_fails_when_the_state_it_watches_changes_and_passes_when_it_does_not(self):
        with unittest.mock.patch.object(real_repo_guard, "real_repo_state", side_effect=[b"a", b"a"]):
            with real_repo_unchanged(self):
                pass
        with unittest.mock.patch.object(real_repo_guard, "real_repo_state", side_effect=[b"a", b"b"]):
            with self.assertRaises(AssertionError):
                with real_repo_unchanged(self):
                    pass

    def test_the_state_it_watches_names_the_submodule_config_and_the_worktree_paths_not_their_heads(self):
        state = real_repo_guard.real_repo_state().decode()
        config, _, worktrees = state.partition("\0")
        self.assertRegex(worktrees, r"(?m)^worktree /")
        self.assertNotIn("HEAD ", worktrees)
        self.assertTrue(config == "" or config.startswith("submodule."), config[:60])


if __name__ == "__main__":
    unittest.main()
