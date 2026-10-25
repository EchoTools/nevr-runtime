"""The embedded version follows the checked-out commit: a commit or a branch switch followed by
`cmake --build` (no explicit configure) re-runs the configure, so the version is never the previous
commit's. Runs cmake/set_project_version_from_git.cmake in a throwaway project and repository."""
from __future__ import annotations

import pathlib
import shutil
import subprocess
import tempfile
import unittest

REPO = pathlib.Path(__file__).resolve().parents[2]
VERSION_CMAKE = REPO / "cmake/set_project_version_from_git.cmake"


def run(*cmd, cwd):
    return subprocess.run(cmd, cwd=cwd, check=True, capture_output=True, text=True)


def git(cwd, *args):
    return run("git", "-c", "user.name=t", "-c", "user.email=t@example.invalid", "-c", "commit.gpgsign=false", "-c", "tag.gpgsign=false",
               *args, cwd=cwd)


@unittest.skipUnless(shutil.which("cmake") and shutil.which("ninja"), "cmake and ninja are required")
class VersionReconfigureTest(unittest.TestCase):
    def setUp(self):
        self.tmp = pathlib.Path(tempfile.mkdtemp(prefix="version-reconfigure-test-", dir="/var/tmp"))
        self.addCleanup(shutil.rmtree, self.tmp)
        self.src = self.tmp / "src"
        self.src.mkdir()
        (self.src / "CMakeLists.txt").write_text(
            "cmake_minimum_required(VERSION 3.20)\n"
            "project(toy NONE)\n"
            f'include("{VERSION_CMAKE}")\n'
            "set_project_version_from_git()\n"
            'file(WRITE "${CMAKE_BINARY_DIR}/version.txt" "${PROJECT_VERSION}")\n'
            "add_custom_target(everything ALL)\n")
        git(self.src, "init", "-q", "-b", "main")
        git(self.src, "add", "CMakeLists.txt")
        git(self.src, "commit", "-q", "-m", "one")
        git(self.src, "tag", "-a", "-m", "v1.2.3", "v1.2.3")
        git(self.src, "commit", "-q", "--allow-empty", "-m", "two")
        self.build = self.tmp / "build"
        run("cmake", "-S", str(self.src), "-B", str(self.build), "-G", "Ninja", cwd=self.tmp)

    def built_version(self):
        run("cmake", "--build", str(self.build), cwd=self.tmp)
        return (self.build / "version.txt").read_text()

    def head(self):
        return git(self.src, "rev-parse", "--short", "HEAD").stdout.strip()

    def test_a_commit_then_build_embeds_the_new_commit(self):
        before = (self.build / "version.txt").read_text()
        self.assertTrue(before.endswith(self.head()), before)
        git(self.src, "commit", "-q", "--allow-empty", "-m", "three")
        after = self.built_version()
        self.assertNotEqual(after, before)
        self.assertEqual(after, f"1.2.4-dev.2+{self.head()}")  # a commit past the tag is a development version

    def test_a_branch_switch_then_build_embeds_the_other_commit(self):
        git(self.src, "switch", "-q", "-c", "other", "HEAD~1")
        self.assertEqual(self.built_version(), f"1.2.4-dev.0+{self.head()}")

    def test_a_pre_release_history_tag_is_ignored_and_the_commit_after_it_rebuilds_as_dev(self):
        # `git describe --long` prints v4.0.0-rc.1-1-g<sha> after a pre-release tag; --exclude "v*-*" keeps
        # such history tags from ever being the base, so the nearest release tag (v1.2.3) still is.
        git(self.src, "tag", "-a", "-m", "history", "v4.0.0-rc.1")
        git(self.src, "commit", "-q", "--allow-empty", "-m", "after the history tag")
        self.assertEqual(self.built_version(), f"1.2.4-dev.2+{self.head()}")

    def test_an_unchanged_commit_does_not_reconfigure(self):
        self.built_version()
        again = run("cmake", "--build", str(self.build), "-v", cwd=self.tmp).stdout
        self.assertNotIn("Re-running CMake", again)


if __name__ == "__main__":
    unittest.main()
