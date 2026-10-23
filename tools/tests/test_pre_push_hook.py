"""tools/git-hooks/pre-push refuses a push whose commits record a shebang script as 100644 (#794)."""
import os
import pathlib
import shutil
import subprocess
import tempfile
import unittest

REPO = pathlib.Path(__file__).resolve().parents[2]


class PrePushHookTest(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self._tmp.cleanup)
        self.tmp = pathlib.Path(self._tmp.name)
        self.env = {**os.environ, "HOME": str(self.tmp), "GIT_CONFIG_GLOBAL": "/dev/null",
                    "GIT_CONFIG_NOSYSTEM": "1", "GIT_TERMINAL_PROMPT": "0"}
        for var in ("GIT_DIR", "GIT_WORK_TREE", "GIT_INDEX_FILE"):
            self.env.pop(var, None)
        self.remote = self.tmp / "remote.git"
        self.run_git("init", "--bare", "-q", "-b", "main", str(self.remote), cwd=self.tmp)
        self.main = self.tmp / "main"
        self.main.mkdir()
        self.run_git("init", "-q", "-b", "main", cwd=self.main)
        for rel in ("tools/git-hooks/pre-push", "tools/install-git-hooks.sh"):
            (self.main / rel).parent.mkdir(parents=True, exist_ok=True)
            shutil.copy(REPO / rel, self.main / rel)
        (self.main / "README").write_text("x\n")
        self.commit("init")
        self.run_git("remote", "add", "origin", str(self.remote), cwd=self.main)
        installed = subprocess.run(["bash", str(self.main / "tools/install-git-hooks.sh")], cwd=self.main,
                                   env=self.env, capture_output=True, text=True)
        self.assertEqual(installed.returncode, 0, installed.stderr)

    def run_git(self, *args, cwd=None, env=None):
        return subprocess.run(["git", "-c", "user.name=t", "-c", "user.email=t@example.com", *args],
                              cwd=cwd or self.main, env=env or self.env, capture_output=True, text=True)

    def commit(self, message):
        self.run_git("add", "-A")
        result = self.run_git("commit", "-q", "-m", message)
        self.assertEqual(result.returncode, 0, result.stderr)

    def add_script(self, mode):
        script = self.main / "tools/run.sh"
        script.write_text("#!/usr/bin/env bash\necho hi\n")
        # `git add -A` in commit() re-reads the disk mode, so the disk carries the mode under test.
        script.chmod(0o755 if mode == "+x" else 0o644)
        self.commit("add script")

    def push(self, *args, env=None):
        return self.run_git("push", "origin", *args, env=env)

    def remote_has(self, ref):
        return self.run_git("rev-parse", "--verify", "-q", ref, cwd=self.remote).returncode == 0

    def test_the_installer_links_the_pre_push_hook(self):
        link = self.main / ".git/hooks/pre-push"
        self.assertTrue(link.is_symlink())
        self.assertEqual(os.path.realpath(link), os.path.realpath(self.main / "tools/git-hooks/pre-push"))

    def test_a_committed_shebang_script_without_the_exec_bit_blocks_the_push(self):
        self.add_script("-x")
        result = self.push("main")
        self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("tools/run.sh starts with #! but is recorded 100644 in refs/heads/main", result.stderr)
        self.assertIn("COMMIT the mode change", result.stderr)
        self.assertFalse(self.remote_has("main"))

    def test_an_executable_script_lets_the_push_through(self):
        self.add_script("+x")
        result = self.push("main")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertTrue(self.remote_has("main"))

    def test_a_mode_fixed_in_the_index_but_not_committed_does_not_pass(self):
        self.add_script("-x")
        self.assertNotEqual(self.push("main").returncode, 0)
        self.run_git("update-index", "--chmod=+x", "tools/run.sh")  # the hook's own advice, without the commit
        (self.main / "tools/run.sh").chmod(0o755)  # the disk follows, as it does for a user
        result = self.push("main")
        self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertFalse(self.remote_has("main"))
        self.commit("record the mode")
        self.assertEqual(self.push("main").returncode, 0)

    def test_a_branch_that_is_not_checked_out_is_checked_too(self):
        self.run_git("checkout", "-q", "-b", "side")
        self.add_script("-x")
        self.run_git("checkout", "-q", "main")  # the checkout is clean; only `side` carries the script
        result = self.push("side")
        self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("refs/heads/side", result.stderr)
        self.assertFalse(self.remote_has("side"))
        self.assertEqual(self.push("main").returncode, 0)

    def test_a_tag_on_a_bad_commit_is_checked_lightweight_and_annotated(self):
        self.add_script("-x")
        self.run_git("tag", "light")
        self.run_git("tag", "-a", "-m", "annotated", "ann")
        for tag in ("light", "ann"):
            result = self.push(f"refs/tags/{tag}")
            self.assertNotEqual(result.returncode, 0, tag + result.stdout + result.stderr)
            self.assertFalse(self.remote_has(f"refs/tags/{tag}"))

    def bad_side_branch(self):
        """A branch `side` carrying a 100644 script; the checkout is back on a clean `main`."""
        self.run_git("checkout", "-q", "-b", "side")
        self.add_script("-x")
        self.run_git("checkout", "-q", "main")

    def test_several_refs_in_one_push_are_all_checked_in_either_order(self):
        self.bad_side_branch()
        for refs in (("main:good", "side"), ("side", "main:good")):
            result = self.push(*refs)
            self.assertNotEqual(result.returncode, 0, " ".join(refs) + result.stdout + result.stderr)
            self.assertIn("refs/heads/side", result.stderr)
            self.assertFalse(self.remote_has("side"))
            self.assertFalse(self.remote_has("good"))

    def test_a_tag_of_a_tree_does_not_end_the_check_of_the_refs_after_it(self):
        self.bad_side_branch()
        self.run_git("tag", "treetag", "main^{tree}")
        for refs in (("refs/tags/treetag", "side"), ("side", "refs/tags/treetag")):
            result = self.push(*refs)
            self.assertNotEqual(result.returncode, 0, " ".join(refs) + result.stdout + result.stderr)
            self.assertIn("refs/heads/side", result.stderr)
            self.assertFalse(self.remote_has("side"))
        self.assertEqual(self.push("refs/tags/treetag").returncode, 0)

    def test_a_note_that_starts_with_a_shebang_is_not_refused(self):
        self.run_git("notes", "add", "-m", "#!/not/a/script")
        result = self.push("refs/notes/commits")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertTrue(self.remote_has("refs/notes/commits"))

    def test_deleting_a_remote_branch_is_allowed_whatever_the_checkout_holds(self):
        self.assertEqual(self.push("main:old").returncode, 0)
        self.add_script("-x")
        result = self.push("--delete", "old")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertFalse(self.remote_has("old"))

    def test_a_missing_python3_refuses_loudly(self):
        bin_dir = self.tmp / "nopython"
        bin_dir.mkdir()
        for tool in ("git", "bash"):
            (bin_dir / tool).symlink_to(shutil.which(tool))
        result = self.push("main", env={**self.env, "PATH": str(bin_dir)})
        self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("python3 is not installed", result.stderr)
        self.assertFalse(self.remote_has("main"))

    def test_a_check_that_crashes_refuses_loudly(self):
        # A pushed commit whose tree object is gone makes `git ls-tree` fail inside the check. The hook
        # is run directly: through `git push`, git's own failure to pack the missing tree would also
        # refuse, and hide a hook that let the crash pass.
        sha = self.run_git("rev-parse", "HEAD").stdout.strip()
        tree = self.run_git("rev-parse", "HEAD^{tree}").stdout.strip()
        (self.main / ".git/objects" / tree[:2] / tree[2:]).unlink()
        result = subprocess.run([str(self.main / ".git/hooks/pre-push"), "origin", str(self.remote)], cwd=self.main,
                                env=self.env, input=f"refs/heads/main {sha} refs/heads/main {'0' * 40}\n",
                                capture_output=True, text=True)
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn("the check itself failed", result.stderr)


if __name__ == "__main__":
    unittest.main()
