"""The fixed build checkout and the one ccache config (#1736).

tools/build_checkout.py decides where a cached build may run: only the fixed checkout, only while `git status --porcelain` is
empty. These tests drive it against real git repositories (a main checkout and the fixed checkout it creates), and read the
justfile for the wiring no test can run (the Quest configure needs the NDK).
"""
import os
import re
import shutil
import stat
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
TOOL = ROOT / "tools" / "build_checkout.py"
CONFIG_SH = ROOT / "tools" / "ccache_config.sh"
JUSTFILE = (ROOT / "justfile").read_text()


def run(args, cwd, env_extra=None, path=None):
    env = {k: v for k, v in os.environ.items() if not k.startswith(("GIT_", "NEVR_"))}
    env.update({"GIT_AUTHOR_NAME": "t", "GIT_AUTHOR_EMAIL": "t@example.com", "GIT_COMMITTER_NAME": "t",
                "GIT_COMMITTER_EMAIL": "t@example.com"})
    env.update(env_extra or {})
    if path is not None:
        env["PATH"] = path
    return subprocess.run(args, cwd=cwd, env=env, capture_output=True, text=True)


def git(cwd, *args):
    done = run(["git", *args], cwd)
    assert done.returncode == 0, done.stderr
    return done.stdout.strip()


class World:
    """A main checkout with two commits, a bare 'origin', and the fixed checkout's path in a temporary work root."""

    def __init__(self, test):
        self.tmp = Path(tempfile.mkdtemp(prefix="build-checkout-test-"))
        test.addCleanup(shutil.rmtree, self.tmp, ignore_errors=True)
        self.origin = self.tmp / "origin.git"
        git(self.tmp, "init", "-q", "--bare", "-b", "main", str(self.origin))
        self.main = self.tmp / "main"
        git(self.tmp, "clone", "-q", str(self.origin), str(self.main))
        (self.main / "a.txt").write_text("one\n")
        git(self.main, "add", "a.txt")
        git(self.main, "commit", "-q", "-m", "one")
        git(self.main, "push", "-q", "origin", "HEAD:refs/heads/main")
        self.first = git(self.main, "rev-parse", "HEAD")
        self.fixed = self.tmp / "work" / "build-checkout" / "nevr-runtime"
        self.env = {"NEVR_BUILD_CHECKOUT": str(self.fixed)}

    def second_commit(self):
        (self.main / "a.txt").write_text("two\n")
        git(self.main, "commit", "-q", "-am", "two")
        git(self.main, "push", "-q", "origin", "HEAD:refs/heads/main")
        return git(self.main, "rev-parse", "HEAD")

    def tool(self, *args, cwd=None):
        return run([sys.executable, "-I", str(TOOL), *args], cwd or self.main, self.env)

    def sync(self, rev="origin/main"):
        return self.tool("sync", rev, cwd=self.main)


class FixedCheckoutTest(unittest.TestCase):
    def test_the_path_is_the_environment_override_or_under_the_work_root(self):
        world = World(self)
        self.assertEqual(world.tool("path").stdout.strip(), str(world.fixed))
        done = run([sys.executable, "-I", str(TOOL), "path"], world.main, {"NEVR_WORK_ROOT": "/w"})
        self.assertEqual(done.stdout.strip(), "/w/build-checkout/nevr-runtime")

    def test_sync_creates_the_checkout_at_the_commit_locks_it_and_writes_its_readme(self):
        world = World(self)
        done = world.sync()
        self.assertEqual(done.returncode, 0, done.stderr)
        self.assertEqual(git(world.fixed, "rev-parse", "HEAD"), world.first)
        self.assertEqual(git(world.fixed, "status", "--porcelain"), "")
        self.assertIn("locked", git(world.main, "worktree", "list", "--porcelain"))
        readme = (world.fixed.parent / "README.md").read_text()
        for word in ("What:", "Whose:", "Removed:"):
            self.assertIn(word, readme)
        self.assertFalse((world.fixed / "README.md").exists(), "a README inside the checkout would make it dirty")

    def test_sync_moves_a_clean_checkout_to_the_new_commit(self):
        world = World(self)
        self.assertEqual(world.sync().returncode, 0)
        second = world.second_commit()
        done = world.sync()
        self.assertEqual(done.returncode, 0, done.stderr)
        self.assertEqual(git(world.fixed, "rev-parse", "HEAD"), second)

    def test_sync_refuses_a_dirty_checkout_names_the_file_and_does_not_move_it(self):
        world = World(self)
        self.assertEqual(world.sync().returncode, 0)
        world.second_commit()
        (world.fixed / "stray.txt").write_text("x\n")
        done = world.sync()
        self.assertEqual(done.returncode, 1)
        self.assertIn("REFUSED", done.stderr)
        self.assertIn("stray.txt", done.stderr)
        self.assertEqual(git(world.fixed, "rev-parse", "HEAD"), world.first)

    def test_sync_refuses_a_commit_that_does_not_exist(self):
        world = World(self)
        done = world.sync("no-such-rev")
        self.assertEqual(done.returncode, 2)
        self.assertFalse(world.fixed.exists())

    def test_is_fixed_is_true_only_in_the_fixed_checkout(self):
        world = World(self)
        self.assertEqual(world.sync().returncode, 0)
        self.assertEqual(world.tool("is-fixed", cwd=world.fixed).returncode, 0)
        self.assertEqual(world.tool("is-fixed", cwd=world.main).returncode, 1)
        (world.fixed / "sub").mkdir()
        self.assertEqual(world.tool("is-fixed", cwd=world.fixed / "sub").returncode, 0, "a subdirectory is the same checkout")

    def test_guard_refuses_only_a_dirty_fixed_checkout(self):
        world = World(self)
        self.assertEqual(world.sync().returncode, 0)
        self.assertEqual(world.tool("guard", cwd=world.fixed).returncode, 0, "clean")
        (world.fixed / "stray.txt").write_text("x\n")
        refused = world.tool("guard", cwd=world.fixed)
        self.assertEqual(refused.returncode, 1)
        self.assertIn("stray.txt", refused.stderr)
        (world.fixed / "stray.txt").unlink()
        (world.fixed / "a.txt").write_text("changed\n")
        self.assertEqual(world.tool("guard", cwd=world.fixed).returncode, 1, "a modified tracked file")
        (world.main / "stray.txt").write_text("x\n")
        self.assertEqual(world.tool("guard", cwd=world.main).returncode, 0, "any other tree is not checked")

    def test_the_quest_launcher_is_kept_only_in_the_clean_fixed_checkout(self):
        world = World(self)
        self.assertEqual(world.sync().returncode, 0)
        kept = world.tool("quest-launcher", "/usr/bin/ccache", cwd=world.fixed)
        self.assertEqual((kept.returncode, kept.stdout.strip()), (0, "/usr/bin/ccache"))
        elsewhere = world.tool("quest-launcher", "/usr/bin/ccache", cwd=world.main)
        self.assertEqual((elsewhere.returncode, elsewhere.stdout.strip()), (0, ""))
        self.assertIn("off", elsewhere.stderr)
        empty = world.tool("quest-launcher", "", cwd=world.fixed)
        self.assertEqual((empty.returncode, empty.stdout.strip()), (0, ""), "no launcher configured stays none")
        (world.fixed / "stray.txt").write_text("x\n")
        dirty = world.tool("quest-launcher", "/usr/bin/ccache", cwd=world.fixed)
        self.assertEqual(dirty.returncode, 1)
        self.assertEqual(dirty.stdout.strip(), "")
        self.assertIn("REFUSED", dirty.stderr)


class CcacheConfigTest(unittest.TestCase):
    def fake_ccache(self):
        tmp = Path(tempfile.mkdtemp(prefix="ccache-config-test-"))
        self.addCleanup(shutil.rmtree, tmp, ignore_errors=True)
        bin_dir = tmp / "bin"
        bin_dir.mkdir()
        tool = bin_dir / "ccache"
        tool.write_text("#!/bin/sh\nexit 0\n")
        tool.chmod(tool.stat().st_mode | stat.S_IXUSR)
        return tmp, f"{bin_dir}:/usr/bin:/bin"

    def test_the_config_is_copied_into_the_cache_directory_and_that_path_is_printed(self):
        tmp, path = self.fake_ccache()
        cache = tmp / "cache"
        done = run([str(CONFIG_SH), str(cache)], ROOT, path=path)
        self.assertEqual(done.returncode, 0, done.stderr)
        self.assertEqual(done.stdout.strip(), str(cache / "ccache.conf"))
        self.assertEqual((cache / "ccache.conf").read_text(), (ROOT / "tools" / "ccache.conf").read_text())

    def test_a_changed_config_replaces_the_copy_and_an_unchanged_one_is_left_alone(self):
        tmp, path = self.fake_ccache()
        cache = tmp / "cache"
        cache.mkdir()
        (cache / "ccache.conf").write_text("max_size = 5G\n")
        run([str(CONFIG_SH), str(cache)], ROOT, path=path)
        self.assertEqual((cache / "ccache.conf").read_text(), (ROOT / "tools" / "ccache.conf").read_text())
        before = (cache / "ccache.conf").stat().st_mtime_ns
        run([str(CONFIG_SH), str(cache)], ROOT, path=path)
        self.assertEqual((cache / "ccache.conf").stat().st_mtime_ns, before)
        self.assertEqual([p.name for p in cache.iterdir()], ["ccache.conf"], "no temporary file is left")

    def test_without_ccache_or_with_an_unwritable_directory_the_repo_config_is_named(self):
        repo_conf = str(ROOT / "tools" / "ccache.conf")
        done = run([str(CONFIG_SH), "/nonexistent-cache"], ROOT, path="/usr/bin:/bin")
        self.assertEqual((done.returncode, done.stdout.strip()), (0, repo_conf), "no ccache on PATH")
        tmp, path = self.fake_ccache()
        blocked = tmp / "blocked"
        blocked.write_text("a file, not a directory")
        done = run([str(CONFIG_SH), str(blocked / "cache")], ROOT, path=path)
        self.assertEqual((done.returncode, done.stdout.strip()), (0, repo_conf), "the cache directory cannot be made")

    def test_the_repo_config_names_a_size_and_keeps_the_cross_worktree_settings(self):
        conf = (ROOT / "tools" / "ccache.conf").read_text()
        self.assertRegex(conf, r"(?m)^max_size = \d+G$")
        self.assertRegex(conf, r"(?m)^hash_dir = false$")


class JustfileWiringTest(unittest.TestCase):
    def recipe(self, name):
        match = re.search(rf"(?ms)^{re.escape(name)}[^\n]*:[^\n]*\n(.*?)(?=^\S)", JUSTFILE)
        self.assertIsNotNone(match, name)
        return match.group(0)

    def test_no_quest_configure_blanks_the_launcher_unconditionally(self):
        self.assertNotRegex(JUSTFILE, r"CMAKE_C_COMPILER_LAUNCHER= CMAKE_CXX_COMPILER_LAUNCHER=",
                            "a Quest configure that always clears the launcher runs every Quest build uncached")
        for name in ("configure-android", "_quest-apk"):
            body = self.recipe(name)
            self.assertIn("tools/build_checkout.py quest-launcher", body, name)
            self.assertIn('CMAKE_CXX_COMPILER_LAUNCHER="$launcher"', body, name)

    def test_the_guard_runs_before_every_configure_that_can_use_the_cache(self):
        self.assertRegex(JUSTFILE, r"(?m)^_build-guard:\n    @python3 -I tools/build_checkout\.py guard$")
        for header in (r"^configure: _build-guard ", r"^configure-android: _build-guard$", r"^_quest-apk store_apk: _build-guard$"):
            self.assertRegex(JUSTFILE, "(?m)" + header)

    def test_ccache_reads_one_config_that_lives_in_the_cache_directory(self):
        self.assertNotRegex(JUSTFILE, r'CCACHE_CONFIGPATH :=\s*justfile_directory\(\)',
                            "a path inside a worktree dies with the worktree and ccache falls back to its 5 GiB default")
        self.assertRegex(JUSTFILE, r"export CCACHE_CONFIGPATH := `tools/ccache_config\.sh ")

    @unittest.skipUnless(shutil.which("just"), "just is not installed")
    def test_a_justfile_without_the_tools_directory_still_evaluates_and_names_the_repo_config(self):
        # tools/tests/test_verify_parallel.py runs a copy of the justfile in a bare directory: the config backtick must not fail there.
        tmp = Path(tempfile.mkdtemp(prefix="justfile-copy-"))
        self.addCleanup(shutil.rmtree, tmp, ignore_errors=True)
        (tmp / "justfile").write_text(JUSTFILE)
        done = run(["just", "--evaluate", "CCACHE_CONFIGPATH"], tmp)
        self.assertEqual(done.returncode, 0, done.stderr)
        self.assertTrue(done.stdout.strip().endswith("/ccache.conf"), done.stdout)

    def test_the_quest_cmake_skips_the_module_scan_under_a_launcher(self):
        cmake = (ROOT / "src" / "quest" / "CMakeLists.txt").read_text()
        self.assertRegex(cmake, r'if\(CMAKE_CXX_COMPILER_LAUNCHER MATCHES "ccache"\)\s*set\(CMAKE_CXX_SCAN_FOR_MODULES OFF\)')


if __name__ == "__main__":
    unittest.main()
