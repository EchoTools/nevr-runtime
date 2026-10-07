"""tools/worktree-setup.sh on a throwaway main checkout and worktree: it never touches the real repo."""
from __future__ import annotations

import os
import pathlib
import shutil
import subprocess
import tempfile
import unittest

REPO = pathlib.Path(__file__).resolve().parents[2]
SCRIPT = REPO / "tools/worktree-setup.sh"
SECRET = "NEVR_API_KEY=do-not-print-me"


def git(cwd, *args):
    subprocess.run(["git", "-c", "user.name=t", "-c", "user.email=t@example.invalid", "-C", str(cwd), *args],
                   check=True, capture_output=True)


class WorktreeSetupTest(unittest.TestCase):
    def setUp(self):
        self.tmp = pathlib.Path(tempfile.mkdtemp(prefix="worktree-setup-test-", dir="/var/tmp"))
        self.addCleanup(shutil.rmtree, self.tmp)
        self.main = self.tmp / "main"
        (self.main / "tools").mkdir(parents=True)
        shutil.copy(SCRIPT, self.main / "tools/worktree-setup.sh")
        git(self.main, "init", "-q")
        git(self.main, "add", "tools/worktree-setup.sh")
        git(self.main, "commit", "-q", "-m", "init", "--no-gpg-sign")
        # The inputs a build needs, untracked like the real submodules/gen/.env in a worktree.
        for name in ("minhook", "breakpad", "lss"):
            d = self.main / "extern" / name
            d.mkdir(parents=True)
            (d / "CMakeLists.txt").write_text(f"# {name}\n")
            (d / ".git").write_text("gitdir: elsewhere\n")
        (self.main / "gen/cpp").mkdir(parents=True)
        (self.main / "gen/cpp/x.pb.cc").write_text("// generated\n")
        (self.main / ".env").write_text(SECRET + "\n")
        self.wt = self.tmp / "wt"
        git(self.main, "worktree", "add", "-q", "--no-track", "-b", "feature", str(self.wt))
        # A real worktree has the submodule directories, empty (a gitlink checks out as an empty directory).
        for name in ("minhook", "breakpad", "lss"):
            (self.wt / "extern" / name).mkdir(parents=True)

    def run_script(self, cwd, *args):
        return subprocess.run([str(cwd / "tools/worktree-setup.sh"), *args], cwd=cwd, capture_output=True, text=True,
                              timeout=60)

    def test_a_fresh_worktree_gets_the_inputs_and_the_main_checkout_is_untouched(self):
        before = sorted(str(p.relative_to(self.main)) for p in self.main.rglob("*") if ".git/" not in str(p))
        result = self.run_script(self.wt)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        for name in ("minhook", "breakpad", "lss"):
            self.assertTrue((self.wt / "extern" / name / "CMakeLists.txt").exists(), name)
            self.assertFalse((self.wt / "extern" / name / ".git").exists(), f"{name}/.git must not be copied")
        self.assertTrue((self.wt / "gen/cpp/x.pb.cc").exists())
        self.assertEqual((self.wt / ".env").read_text(), SECRET + "\n")
        after = sorted(str(p.relative_to(self.main)) for p in self.main.rglob("*") if ".git/" not in str(p))
        self.assertEqual(before, after, "the main checkout changed")

    def test_the_secret_is_never_printed(self):
        result = self.run_script(self.wt)
        self.assertNotIn("do-not-print-me", result.stdout + result.stderr)

    def test_check_names_what_is_missing_and_the_fix(self):
        result = self.run_script(self.wt, "--check")
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn("extern/minhook", result.stderr)
        self.assertIn("gen/", result.stderr)
        self.assertIn("just worktree-setup", result.stderr)

    def test_check_passes_after_setup(self):
        self.assertEqual(self.run_script(self.wt).returncode, 0)
        self.assertEqual(self.run_script(self.wt, "--check").returncode, 0)

    def test_running_it_in_the_main_checkout_is_refused(self):
        result = self.run_script(self.main)
        self.assertEqual(result.returncode, 2, result.stdout + result.stderr)
        self.assertIn("main checkout", result.stderr)

    def test_missing_gen_in_the_main_checkout_is_a_clear_error(self):
        shutil.rmtree(self.main / "gen")
        result = self.run_script(self.wt)
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn("just proto", result.stderr)
        self.assertFalse((self.wt / "extern/minhook/CMakeLists.txt").exists(), "nothing is copied on error")

    def test_a_missing_env_is_a_warning_not_a_failure(self):
        (self.main / ".env").unlink()
        result = self.run_script(self.wt)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("no .env", result.stderr)
        self.assertFalse((self.wt / ".env").exists())

    def test_running_it_twice_is_safe_and_does_not_nest_gen(self):
        self.assertEqual(self.run_script(self.wt).returncode, 0)
        self.assertEqual(self.run_script(self.wt).returncode, 0)
        self.assertFalse((self.wt / "gen/gen").exists())
        self.assertTrue((self.wt / "gen/cpp/x.pb.cc").exists())

    def test_empty_submodule_directories_count_as_missing_for_check(self):
        result = self.run_script(self.wt, "--check")
        self.assertEqual(result.returncode, 1)
        self.assertIn("extern/minhook", result.stderr)

    def test_a_submodule_holding_only_its_git_pointer_is_not_initialised(self):
        (self.wt / "extern/minhook/.git").write_text("gitdir: elsewhere\n")
        self.assertEqual(self.run_script(self.wt, "--check").returncode, 1)
        for p in (self.main / "extern/lss").iterdir():
            if p.name != ".git":
                p.unlink()
        result = self.run_script(self.wt)
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn("not initialised", result.stderr)

    def test_a_symlinked_path_to_the_main_checkout_is_still_refused_and_nothing_is_deleted(self):
        link = self.tmp / "main-link"
        link.symlink_to(self.main)
        result = self.run_script(link)
        self.assertEqual(result.returncode, 2, result.stdout + result.stderr)
        self.assertTrue((self.main / "gen/cpp/x.pb.cc").exists(), "the main checkout's gen/ was deleted")

    def test_outside_a_git_checkout_it_fails_before_touching_anything(self):
        loose = self.tmp / "loose"
        (loose / "tools").mkdir(parents=True)
        shutil.copy(SCRIPT, loose / "tools/worktree-setup.sh")
        (loose / "gen/cpp").mkdir(parents=True)
        (loose / "gen/cpp/own.pb.cc").write_text("own\n")
        env = dict(os.environ, GIT_CEILING_DIRECTORIES=str(self.tmp))
        result = subprocess.run([str(loose / "tools/worktree-setup.sh")], cwd=loose, capture_output=True, text=True,
                                env=env, timeout=60)
        self.assertEqual(result.returncode, 2, result.stdout + result.stderr)
        self.assertIn("not inside a git checkout", result.stderr)
        self.assertTrue((loose / "gen/cpp/own.pb.cc").exists())

    def test_a_gen_with_other_content_is_refused_not_replaced(self):
        (self.wt / "gen/go").mkdir(parents=True)
        (self.wt / "gen/go/own.go").write_text("own\n")
        (self.wt / "gen/.dot").write_text("own\n")
        result = self.run_script(self.wt)
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn("not touched: gen/", result.stderr)
        self.assertEqual((self.wt / "gen/go/own.go").read_text(), "own\n")
        self.assertEqual((self.wt / "gen/.dot").read_text(), "own\n")

    def test_a_failed_copy_leaves_no_partial_gen_and_no_temporary(self):
        unreadable = self.main / "gen/cpp/secret.pb.cc"
        unreadable.write_text("x\n")
        unreadable.chmod(0)
        self.addCleanup(unreadable.chmod, 0o644)
        if os.access(unreadable, os.R_OK):
            self.skipTest("running as a user that can read mode-0 files")
        result = self.run_script(self.wt)
        self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertFalse((self.wt / "gen").exists(), "a partial gen/ was left in place")
        scratch = pathlib.Path(subprocess.check_output(
            ["git", "-C", str(self.wt), "rev-parse", "--path-format=absolute", "--git-dir"], text=True).strip())
        self.assertFalse((scratch / "worktree-setup/tmp").exists(), "temporary copy left behind")

    def test_an_existing_env_is_kept_and_a_new_one_is_private(self):
        (self.wt / ".env").write_text("OWN=1\n")
        result = self.run_script(self.wt)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual((self.wt / ".env").read_text(), "OWN=1\n")
        self.assertIn("kept what this worktree already has: .env", result.stdout)
        (self.wt / ".env").unlink()
        self.assertEqual(self.run_script(self.wt).returncode, 0)
        self.assertEqual((self.wt / ".env").stat().st_mode & 0o777, 0o600)

    def test_a_worktrees_own_initialised_submodule_and_gen_are_never_replaced(self):
        (self.wt / "extern/minhook").mkdir(exist_ok=True)
        (self.wt / "extern/minhook/own.txt").write_text("local work\n")
        (self.wt / "gen/cpp").mkdir(parents=True)
        (self.wt / "gen/cpp/own.pb.cc").write_text("own\n")
        result = self.run_script(self.wt)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual((self.wt / "extern/minhook/own.txt").read_text(), "local work\n")
        self.assertFalse((self.wt / "extern/minhook/CMakeLists.txt").exists())
        self.assertEqual((self.wt / "gen/cpp/own.pb.cc").read_text(), "own\n")
        self.assertIn("kept what this worktree already has", result.stdout)
        self.assertTrue((self.wt / "extern/breakpad/CMakeLists.txt").exists(), "the missing ones are still filled")

    def test_a_submodule_with_a_git_directory_only_is_not_initialised(self):
        for d in ("minhook",):
            shutil.rmtree(self.main / "extern" / d)
            (self.main / "extern" / d / ".git").mkdir(parents=True)
            (self.main / "extern" / d / ".git/HEAD").write_text("ref: x\n")
        result = self.run_script(self.wt)
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn("not initialised", result.stderr)
        (self.wt / "extern/minhook/.git").mkdir(parents=True, exist_ok=True)
        (self.wt / "extern/minhook/.git/HEAD").write_text("ref: x\n")
        self.assertEqual(self.run_script(self.wt, "--check").returncode, 1)

    def test_an_override_can_never_aim_the_script_at_the_main_checkout(self):
        other = self.tmp / "other"
        shutil.copytree(self.main / "extern", other / "extern")
        (other / "gen/cpp").mkdir(parents=True)
        (other / "gen/cpp/o.pb.cc").write_text("o\n")
        before = sorted(str(p.relative_to(self.main)) for p in self.main.rglob("*") if ".git/" not in str(p))
        env = dict(os.environ, NEVR_MAIN_CHECKOUT=str(other))
        result = subprocess.run([str(self.main / "tools/worktree-setup.sh")], cwd=self.main, capture_output=True,
                                text=True, env=env, timeout=60)
        self.assertEqual(result.returncode, 2, result.stdout + result.stderr)
        after = sorted(str(p.relative_to(self.main)) for p in self.main.rglob("*") if ".git/" not in str(p))
        self.assertEqual(before, after)

    def test_a_symlinked_extern_is_refused_and_the_target_is_untouched(self):
        shutil.rmtree(self.wt / "extern")
        (self.wt / "extern").symlink_to(self.main / "extern")
        result = self.run_script(self.wt)
        self.assertEqual(result.returncode, 2, result.stdout + result.stderr)
        self.assertIn("symlink", result.stderr)
        for name in ("minhook", "breakpad", "lss"):
            self.assertTrue((self.main / "extern" / name / "CMakeLists.txt").exists(), name)

    def gitdir(self):
        return pathlib.Path(subprocess.check_output(
            ["git", "-C", str(self.wt), "rev-parse", "--path-format=absolute", "--git-dir"], text=True).strip())

    def test_scratch_from_a_killed_run_is_removed_and_user_directories_are_never_touched(self):
        scratch = self.gitdir() / "worktree-setup/tmp/minhook"
        scratch.mkdir(parents=True)
        (scratch / "stale").write_text("x\n")
        for mine in (".gen.backup", ".gen.abcdef", ".gen.??????"):
            (self.wt / mine).mkdir()
            (self.wt / mine / "keep").write_text("mine\n")
        (self.wt / "extern/.lss.orig12").mkdir()
        (self.wt / "extern/.lss.orig12/keep").write_text("mine\n")
        self.assertEqual(self.run_script(self.wt).returncode, 0)
        self.assertFalse(scratch.exists())
        for mine in (".gen.backup", ".gen.abcdef", ".gen.??????", "extern/.lss.orig12"):
            self.assertEqual((self.wt / mine / "keep").read_text(), "mine\n", mine)

    def test_git_environment_cannot_make_the_main_checkout_look_like_a_linked_worktree(self):
        # GIT_DIR naming a LINKED worktree's git dir would make `git rev-parse --git-dir` differ from the
        # common dir even when the script sits in the main checkout.
        env = dict(os.environ, GIT_DIR=str(self.gitdir()), GIT_WORK_TREE=str(self.wt))
        result = subprocess.run([str(self.main / "tools/worktree-setup.sh")], cwd=self.main, capture_output=True,
                                text=True, env=env, timeout=60)
        self.assertEqual(result.returncode, 2, result.stdout + result.stderr)
        self.assertIn("not a linked worktree", result.stderr)

    def test_a_main_checkout_with_a_separate_git_dir_is_still_refused_under_an_override(self):
        sep = self.tmp / "sepmain"
        subprocess.run(["git", "clone", "-q", "--separate-git-dir", str(self.tmp / "sep.git"), str(self.main), str(sep)],
                       check=True, capture_output=True)
        shutil.copytree(self.main / "extern", sep / "extern", dirs_exist_ok=True)
        (sep / "gen").mkdir()
        (sep / "gen/NOTES").write_text("hand written\n")
        env = dict(os.environ, NEVR_MAIN_CHECKOUT=str(self.wt))
        result = subprocess.run([str(sep / "tools/worktree-setup.sh")], cwd=sep, capture_output=True, text=True,
                                env=env, timeout=60)
        self.assertEqual(result.returncode, 2, result.stdout + result.stderr)
        self.assertEqual((sep / "gen/NOTES").read_text(), "hand written\n")
        self.assertFalse((sep / ".env").exists())

    def test_the_script_acts_on_the_checkout_it_lives_in_even_when_invoked_through_a_symlink(self):
        home = self.tmp / "home"
        (home / "bin").mkdir(parents=True)
        (home / "bin/ws").symlink_to(self.wt / "tools/worktree-setup.sh")
        (home / "gen").mkdir()
        (home / "gen/data.txt").write_text("mine\n")
        env = dict(os.environ, NEVR_MAIN_CHECKOUT=str(self.main))
        result = subprocess.run([str(home / "bin/ws")], cwd=home, capture_output=True, text=True, env=env, timeout=60)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertTrue((self.wt / "extern/minhook/CMakeLists.txt").exists())
        self.assertEqual((home / "gen/data.txt").read_text(), "mine\n")
        self.assertFalse((home / "extern").exists())
        self.assertFalse((home / ".env").exists())

    def test_dangling_symlink_destinations_are_refused(self):
        for victim in ("gen", ".env", "extern/lss"):
            link = self.wt / victim
            if link.exists() or link.is_symlink():
                shutil.rmtree(link) if link.is_dir() and not link.is_symlink() else link.unlink()
            link.symlink_to(self.tmp / "nowhere")
            result = self.run_script(self.wt)
            self.assertEqual(result.returncode, 2, f"{victim}: {result.stdout}{result.stderr}")
            self.assertIn("symlink", result.stderr)
            link.unlink()
            if victim == "extern/lss":
                link.mkdir()

    def test_simultaneous_runs_do_not_nest_copies_or_fail_halfway(self):
        runs = [subprocess.Popen([str(self.wt / "tools/worktree-setup.sh")], cwd=self.wt, stdout=subprocess.PIPE,
                                 stderr=subprocess.PIPE, text=True) for _ in range(8)]
        codes = []
        for r in runs:
            r.communicate(timeout=60)
            codes.append(r.returncode)
        self.assertTrue(set(codes) <= {0, 1}, codes)
        self.assertIn(0, codes)
        for name in ("minhook", "breakpad", "lss"):
            self.assertTrue((self.wt / "extern" / name / "CMakeLists.txt").exists(), name)
            self.assertEqual([p.name for p in (self.wt / "extern" / name).iterdir()], ["CMakeLists.txt"], name)
        self.assertEqual(sorted(p.name for p in self.wt.glob("extern/.*")), [])
        self.assertTrue((self.wt / "gen/cpp/x.pb.cc").exists())
        self.assertFalse((self.wt / "gen/gen").exists())

    def test_directories_with_only_empty_subdirectories_do_not_pass_the_check(self):
        (self.wt / "extern/minhook/src").mkdir(parents=True)
        (self.wt / "gen/cpp").mkdir(parents=True)
        (self.wt / "gen/cpp/.gitkeep").write_text("")
        result = self.run_script(self.wt, "--check")
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn("extern/minhook", result.stderr)
        self.assertIn("gen/cpp", result.stderr)

    def test_gen_keeps_the_main_checkouts_mode(self):
        (self.main / "gen").chmod(0o755)
        self.assertEqual(self.run_script(self.wt).returncode, 0)
        self.assertEqual((self.wt / "gen").stat().st_mode & 0o777, 0o755)

    def test_a_bare_layout_needs_the_override_then_works(self):
        bare = self.tmp / "proj"
        bare.mkdir()
        subprocess.run(["git", "clone", "-q", "--bare", str(self.main), str(bare / ".bare")], check=True,
                       capture_output=True)
        wt = bare / "wt"
        git(bare / ".bare", "worktree", "add", "-q", "--no-track", "-b", "b", str(wt))
        for name in ("minhook", "breakpad", "lss"):
            (wt / "extern" / name).mkdir(parents=True, exist_ok=True)
        refused = subprocess.run([str(wt / "tools/worktree-setup.sh")], cwd=wt, capture_output=True, text=True,
                                 timeout=60)
        self.assertEqual(refused.returncode, 2, refused.stdout + refused.stderr)
        self.assertIn("NEVR_MAIN_CHECKOUT", refused.stderr)
        env = dict(os.environ, NEVR_MAIN_CHECKOUT=str(self.main))
        ok = subprocess.run([str(wt / "tools/worktree-setup.sh")], cwd=wt, capture_output=True, text=True, env=env,
                            timeout=60)
        self.assertEqual(ok.returncode, 0, ok.stdout + ok.stderr)
        self.assertTrue((wt / "extern/minhook/CMakeLists.txt").exists())

    def test_check_needs_minhook_and_a_generated_source_but_not_the_quest_submodules(self):
        shutil.copytree(self.main / "extern/minhook", self.wt / "extern/minhook", dirs_exist_ok=True)
        (self.wt / "gen/cpp").mkdir(parents=True)
        self.assertEqual(self.run_script(self.wt, "--check").returncode, 1)  # gen/cpp holds no file
        (self.wt / "gen/cpp/g.pb.cc").write_text("g\n")
        self.assertEqual(self.run_script(self.wt, "--check").returncode, 0)  # breakpad and lss still empty
        shutil.rmtree(self.wt / "extern/minhook")
        (self.wt / "extern/minhook").mkdir()
        self.assertEqual(self.run_script(self.wt, "--check").returncode, 1)

    def test_an_unknown_argument_is_rejected(self):
        self.assertEqual(self.run_script(self.wt, "--check", "bogus").returncode, 2)
        self.assertEqual(self.run_script(self.wt, "bogus").returncode, 2)

    def test_a_bare_layout_needs_the_main_checkout_named(self):
        env = dict(os.environ, NEVR_MAIN_CHECKOUT=str(self.main))
        result = subprocess.run([str(self.wt / "tools/worktree-setup.sh")], cwd=self.wt, capture_output=True,
                                text=True, env=env, timeout=60)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        bad = dict(os.environ, NEVR_MAIN_CHECKOUT=str(self.tmp / "nowhere"))
        result = subprocess.run([str(self.wt / "tools/worktree-setup.sh")], cwd=self.wt, capture_output=True,
                                text=True, env=bad, timeout=60)
        self.assertEqual(result.returncode, 2)


if __name__ == "__main__":
    unittest.main()
