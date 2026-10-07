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

    def test_running_it_twice_is_safe(self):
        self.assertEqual(self.run_script(self.wt).returncode, 0)
        self.assertEqual(self.run_script(self.wt).returncode, 0)


if __name__ == "__main__":
    unittest.main()
