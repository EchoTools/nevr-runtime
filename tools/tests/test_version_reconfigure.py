"""The build's stamp follows the checked-out commit without a configure (#1935): a commit or a branch switch followed by
`cmake --build` rewrites the one generated stamp unit (cmake/nevr_build_stamp.cmake, run by nevr_build_stamp_refresh at
every build), and the configure does not run again. PROJECT_VERSION is the committed VERSION file, so it does not
change with a commit at all. Runs the real cmake files in a throwaway project and repository."""
from __future__ import annotations

import pathlib
import shutil
import subprocess
import tempfile
import unittest

REPO = pathlib.Path(__file__).resolve().parents[2]
VERSION_CMAKE = REPO / "cmake/set_project_version_from_git.cmake"
TARGETS_CMAKE = REPO / "cmake/nevr_build_stamp_targets.cmake"
STAMP_SCRIPT = REPO / "cmake/nevr_build_stamp.cmake"


def run(*cmd, cwd):
    return subprocess.run(cmd, cwd=cwd, check=True, capture_output=True, text=True)


def git(cwd, *args):
    return run("git", "-c", "user.name=t", "-c", "user.email=t@example.invalid", "-c", "commit.gpgsign=false", "-c", "tag.gpgsign=false",
               *args, cwd=cwd)


@unittest.skipUnless(shutil.which("cmake") and shutil.which("ninja") and shutil.which("git"), "cmake, ninja and git are required")
class VersionReconfigureTest(unittest.TestCase):
    def setUp(self):
        self.tmp = pathlib.Path(tempfile.mkdtemp(prefix="version-reconfigure-test-", dir="/var/tmp"))
        self.addCleanup(shutil.rmtree, self.tmp, ignore_errors=True)
        self.src = self.tmp / "src"
        self.src.mkdir()
        (self.src / "VERSION").write_text("1.2.3\n")
        (self.src / "src" / "core").mkdir(parents=True)
        shutil.copy(REPO / "src" / "core" / "build_stamp.h", self.src / "src" / "core" / "build_stamp.h")
        (self.src / "CMakeLists.txt").write_text(
            "cmake_minimum_required(VERSION 3.20)\n"
            "project(toy CXX)\n"
            "find_package(Git QUIET)\n"
            f'include("{VERSION_CMAKE}")\n'
            "set_project_version_from_git()\n"
            f'include("{TARGETS_CMAKE}")\n'
            f'nevr_build_stamp_targets(REPO_DIR "${{CMAKE_CURRENT_SOURCE_DIR}}")\n'
            'file(WRITE "${CMAKE_BINARY_DIR}/version.txt" "${PROJECT_VERSION}")\n'
            "add_custom_target(everything ALL)\n"
            "add_dependencies(everything nevr_build_stamp)\n")
        git(self.src, "init", "-q", "-b", "main")
        git(self.src, "add", "CMakeLists.txt", "VERSION", "src")
        git(self.src, "commit", "-q", "-m", "one")
        git(self.src, "tag", "-a", "-m", "v1.2.2", "v1.2.2")
        git(self.src, "commit", "-q", "--allow-empty", "-m", "two")
        self.build = self.tmp / "build"
        run("cmake", "-S", str(self.src), "-B", str(self.build), "-G", "Ninja", cwd=self.tmp)
        self.stamp = self.build / "generated" / "nevr_build_stamp.cpp"

    def built_stamp(self):
        """The stamp the build wrote: kProjectVersion of the generated unit, after a build."""
        run("cmake", "--build", str(self.build), cwd=self.tmp)
        text = self.stamp.read_text()
        return text.split('kProjectVersion[] = "', 1)[1].split('"', 1)[0]

    def head(self):
        return git(self.src, "rev-parse", "--short", "HEAD").stdout.strip()

    def test_project_version_is_the_version_file_and_no_configure_reads_git_for_it(self):
        self.assertEqual((self.build / "version.txt").read_text(), "1.2.3")
        git(self.src, "commit", "-q", "--allow-empty", "-m", "three")
        self.assertEqual(self.built_stamp(), f"1.2.3-dev.2+{self.head()}")
        self.assertEqual((self.build / "version.txt").read_text(), "1.2.3")

    def test_a_commit_then_build_embeds_the_new_commit(self):
        self.assertEqual(self.built_stamp(), f"1.2.3-dev.1+{self.head()}")
        git(self.src, "commit", "-q", "--allow-empty", "-m", "three")
        self.assertEqual(self.built_stamp(), f"1.2.3-dev.2+{self.head()}")  # a commit past the tag is a development version

    def test_a_branch_switch_then_build_embeds_the_other_commit(self):
        git(self.src, "switch", "-q", "-c", "other", "HEAD~1")
        self.assertEqual(self.built_stamp(), f"1.2.3-dev.0+{self.head()}")

    def test_a_pre_release_history_tag_is_ignored_and_the_commit_after_it_rebuilds_as_dev(self):
        # `git describe --long` prints v4.0.0-rc.1-1-g<sha> after a pre-release tag; --exclude "v*-*" keeps
        # such history tags from ever being the base, so the nearest release tag (v1.2.2) still is.
        git(self.src, "tag", "-a", "-m", "history", "v4.0.0-rc.1")
        git(self.src, "commit", "-q", "--allow-empty", "-m", "after the history tag")
        self.assertEqual(self.built_stamp(), f"1.2.3-dev.2+{self.head()}")

    def test_an_unchanged_commit_does_not_reconfigure(self):
        self.built_stamp()
        again = run("cmake", "--build", str(self.build), "-v", cwd=self.tmp).stdout
        self.assertNotIn("Re-running CMake", again)


if __name__ == "__main__":
    unittest.main()
