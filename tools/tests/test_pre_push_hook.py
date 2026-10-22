"""tools/git-hooks/pre-push refuses a push that carries a shebang script recorded as 100644 (#794)."""
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
        tmp = pathlib.Path(self._tmp.name)
        self.env = {**os.environ, "HOME": str(tmp), "GIT_CONFIG_GLOBAL": "/dev/null", "GIT_CONFIG_NOSYSTEM": "1",
                    "GIT_TERMINAL_PROMPT": "0"}
        for var in ("GIT_DIR", "GIT_WORK_TREE", "GIT_INDEX_FILE"):
            self.env.pop(var, None)
        self.remote = tmp / "remote.git"
        self.run_git("init", "--bare", "-q", "-b", "main", str(self.remote), cwd=tmp)
        self.main = tmp / "main"
        self.main.mkdir()
        self.run_git("init", "-q", "-b", "main", cwd=self.main)
        for rel in ("tools/git-hooks/pre-push", "tools/install-git-hooks.sh", "tools/tests/test_executable_scripts.py"):
            (self.main / rel).parent.mkdir(parents=True, exist_ok=True)
            shutil.copy(REPO / rel, self.main / rel)
        # test_executable_scripts refuses a listing of 100 files or fewer (a blind sensor).
        for i in range(120):
            (self.main / f"filler{i}.txt").write_text("x\n")
        self.commit("init")
        self.run_git("remote", "add", "origin", str(self.remote), cwd=self.main)
        installed = subprocess.run(["bash", str(self.main / "tools/install-git-hooks.sh")], cwd=self.main,
                                   env=self.env, capture_output=True, text=True)
        self.assertEqual(installed.returncode, 0, installed.stderr)

    def run_git(self, *args, cwd):
        return subprocess.run(["git", "-c", "user.name=t", "-c", "user.email=t@example.com", *args], cwd=cwd,
                              env=self.env, capture_output=True, text=True)

    def commit(self, message):
        self.run_git("add", "-A", cwd=self.main)
        result = self.run_git("commit", "-q", "-m", message, cwd=self.main)
        self.assertEqual(result.returncode, 0, result.stderr)

    def add_script(self, mode):
        script = self.main / "tools/run.sh"
        script.write_text("#!/usr/bin/env bash\necho hi\n")
        script.chmod(0o755 if mode == "+x" else 0o644)  # `git add -A` in commit() re-reads the disk mode
        self.run_git("add", "tools/run.sh", cwd=self.main)
        self.run_git("update-index", f"--chmod={mode}", "tools/run.sh", cwd=self.main)
        self.commit("add script")

    def push(self):
        return self.run_git("push", "origin", "main", cwd=self.main)

    def remote_has_main(self):
        return self.run_git("rev-parse", "--verify", "-q", "main", cwd=self.remote).returncode == 0

    def test_the_installer_links_the_pre_push_hook(self):
        link = self.main / ".git/hooks/pre-push"
        self.assertTrue(link.is_symlink())
        self.assertEqual(os.path.realpath(link), os.path.realpath(self.main / "tools/git-hooks/pre-push"))

    def test_a_shebang_script_recorded_without_the_exec_bit_blocks_the_push(self):
        self.add_script("-x")
        result = self.push()
        self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("tools/run.sh", result.stderr)
        self.assertIn("pre-push: refusing the push", result.stderr)
        self.assertFalse(self.remote_has_main())

    def test_an_executable_script_lets_the_push_through(self):
        self.add_script("+x")
        result = self.push()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertTrue(self.remote_has_main())

    def test_a_tree_without_the_check_refuses_loudly(self):
        self.run_git("rm", "-q", "tools/tests/test_executable_scripts.py", cwd=self.main)
        self.commit("drop the check")
        result = self.push()
        self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("merge origin/main", result.stderr)
        self.assertFalse(self.remote_has_main())


if __name__ == "__main__":
    unittest.main()
