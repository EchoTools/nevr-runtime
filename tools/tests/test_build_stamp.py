"""cmake/nevr_build_stamp.cmake: the build's version and commit, written at build time into ONE generated unit (#1935).

The version is the committed VERSION file (exactly X.Y.Z). The commit is what git says when the stamp is written:
`git describe --always --dirty --broken --abbrev=12`, the full and the short commit, and the distance from the nearest
release tag. A release is a CI build exactly on the tag vX.Y.Z of VERSION: it stamps X.Y.Z and refuses a describe
string that ends in -dirty. Every other build stamps X.Y.Z-dev.<distance>+<short commit>.

The REAL script runs, in scratch git repositories, with real tags. tools/build_version.py is the Python mirror that
the packaging tools use; a parity test checks it against the script on the same repositories.
"""
from __future__ import annotations

import os
import pathlib
import re
import shutil
import subprocess
import sys
import tempfile
import time
import unittest

REPO = pathlib.Path(__file__).resolve().parents[2]
STAMP_CMAKE = REPO / "cmake" / "nevr_build_stamp.cmake"
sys.path.insert(0, str(REPO / "tools"))

CI = {"GITHUB_ACTIONS": "true", "GITHUB_REF_TYPE": "tag"}


def git(cwd, *args):
    return subprocess.run(["git", "-c", "user.name=t", "-c", "user.email=t@example.invalid",
                           "-c", "commit.gpgsign=false", "-c", "tag.gpgsign=false", *args],
                          cwd=cwd, check=True, capture_output=True, text=True)


def clean_env(**env):
    base = {k: v for k, v in os.environ.items() if not k.startswith("GITHUB_")}
    base.update(env)
    return base


class Scratch(unittest.TestCase):
    """A scratch repository with a VERSION file, and the stamp script run over it."""

    def setUp(self):
        self.tmp = pathlib.Path(tempfile.mkdtemp(prefix="build-stamp-test-", dir="/var/tmp"))
        self.addCleanup(shutil.rmtree, self.tmp, ignore_errors=True)
        self.src = self.tmp / "src"
        self.src.mkdir()
        git(self.src, "init", "-q", "-b", "main")
        self.write_version("1.2.3")
        (self.src / "tracked.txt").write_text("one\n")
        git(self.src, "add", "VERSION", "tracked.txt")
        git(self.src, "commit", "-q", "-m", "one")
        self.out = self.tmp / "out" / "nevr_build_stamp.cpp"
        self.runs = 0

    def write_version(self, text):
        (self.src / "VERSION").write_text(text)

    def tag(self, name):
        git(self.src, "tag", "-a", "-m", name, name)

    def child(self):
        git(self.src, "commit", "-q", "--allow-empty", "-m", "child")

    def sha(self):
        return git(self.src, "rev-parse", "--short", "HEAD").stdout.strip()

    def describe(self):
        return git(self.src, "describe", "--always", "--dirty", "--broken", "--abbrev=12").stdout.strip()

    def stamp(self, **env):
        """(returncode, values, text) of one run of the script; values is the accessor table, or None."""
        self.runs += 1
        cmd = ["cmake", f"-DNEVR_STAMP_SOURCE_DIR={self.src}", f"-DNEVR_STAMP_OUTPUT={self.out}",
               "-P", str(STAMP_CMAKE)]
        result = subprocess.run(cmd, capture_output=True, text=True, env=clean_env(**env))
        text = " ".join((result.stdout + result.stderr).split())
        values = None
        if result.returncode == 0 and self.out.exists():
            values = parse_values(self.out.read_text())
        return result.returncode, values, text

    def ok(self, **env):
        rc, values, text = self.stamp(**env)
        self.assertEqual(rc, 0, text)
        return values


def parse_values(text):
    """The accessor table of the generated unit: `const char kProjectVersion[] = "...";` and the like."""
    values = {}
    for name, value in re.findall(r'const char (k\w+)\[\] = "((?:[^"\\]|\\.)*)";', text):
        values[name] = value
    for name, value in re.findall(r"const bool (k\w+) = (true|false);", text):
        values[name] = value == "true"
    for name, value in re.findall(r"const int (k\w+) = (-?\d+);", text):
        values[name] = int(value)
    return values


@unittest.skipUnless(shutil.which("cmake") and shutil.which("git"), "cmake and git are required")
class StampScriptTest(Scratch):
    def test_a_dev_build_stamps_the_version_file_with_the_distance_and_the_commit(self):
        self.tag("v1.2.2")
        self.child()
        self.child()
        v = self.ok()
        self.assertEqual(v["kVersion"], "1.2.3")
        self.assertEqual(v["kProjectVersion"], f"1.2.3-dev.2+{self.sha()}")
        self.assertEqual(v["kDescribe"], self.describe())
        self.assertIs(v["kDirty"], False)
        self.assertIs(v["kRelease"], False)

    def test_the_commit_fields_are_the_full_and_the_short_commit(self):
        self.tag("v1.2.2")
        v = self.ok()
        full = git(self.src, "rev-parse", "HEAD").stdout.strip()
        self.assertEqual(v["kCommitFull"], full)
        self.assertEqual(v["kCommitShort"], self.sha())
        self.assertIn(f"\\0NEVR-BUILD {v['kProjectVersion']} {full}\"", self.out.read_text())

    def test_a_ci_build_on_the_release_tag_stamps_exactly_the_version(self):
        self.tag("v1.2.3")
        v = self.ok(**CI, GITHUB_REF_NAME="v1.2.3")
        self.assertEqual(v["kProjectVersion"], "1.2.3")
        self.assertIs(v["kRelease"], True)
        self.assertIs(v["kDirty"], False)

    def test_a_ci_build_on_the_tag_refuses_a_dirty_tree(self):
        self.tag("v1.2.3")
        (self.src / "tracked.txt").write_text("edited\n")
        rc, values, text = self.stamp(**CI, GITHUB_REF_NAME="v1.2.3")
        self.assertNotEqual(rc, 0, text)
        self.assertIsNone(values)
        self.assertIn("-dirty", text)
        self.assertIn("refuses", text)

    def test_a_dirty_dev_build_says_so_in_the_describe_string_and_keeps_the_version(self):
        self.tag("v1.2.2")
        self.child()
        (self.src / "tracked.txt").write_text("edited\n")
        v = self.ok()
        self.assertTrue(v["kDescribe"].endswith("-dirty"), v["kDescribe"])
        self.assertIs(v["kDirty"], True)
        self.assertEqual(v["kProjectVersion"], f"1.2.3-dev.1+{self.sha()}")

    def test_the_wrong_ci_ref_never_stamps_a_release(self):
        self.tag("v1.2.3")
        dev = f"1.2.3-dev.0+{self.sha()}"
        cases = {
            "branch ref": dict(CI, GITHUB_REF_TYPE="branch", GITHUB_REF_NAME="main"),
            "branch named like the tag": dict(CI, GITHUB_REF_TYPE="branch", GITHUB_REF_NAME="v1.2.3"),
            "no GITHUB_ACTIONS": {"GITHUB_REF_TYPE": "tag", "GITHUB_REF_NAME": "v1.2.3"},
            "GITHUB_ACTIONS not exactly true": dict(CI, GITHUB_ACTIONS="TRUE", GITHUB_REF_NAME="v1.2.3"),
            "refs/tags prefix": dict(CI, GITHUB_REF_NAME="refs/tags/v1.2.3"),
        }
        for name, env in cases.items():
            with self.subTest(name):
                self.assertEqual(self.ok(**env)["kProjectVersion"], dev)

    def test_a_ci_tag_that_is_not_the_version_file_is_not_a_release(self):
        self.tag("v1.2.3")
        self.assertEqual(self.ok(**CI, GITHUB_REF_NAME="v1.2.4")["kProjectVersion"], f"1.2.3-dev.0+{self.sha()}")

    def test_a_version_file_that_is_not_exactly_xyz_fails_loudly(self):
        self.tag("v1.2.2")
        self.child()
        for text in ("1.2.3-dev\n", "v1.2.3\n", "1.2\n", "01.2.3\n", "1.2.3+build\n", "1.2.3 \n", "", "1.2.3\n\n"):
            with self.subTest(text=text):
                self.write_version(text)
                rc, values, out = self.stamp()
                self.assertNotEqual(rc, 0, out)
                self.assertIsNone(values)
                self.assertIn("VERSION", out)

    def test_a_missing_version_file_fails_loudly(self):
        (self.src / "VERSION").unlink()
        rc, values, out = self.stamp()
        self.assertNotEqual(rc, 0, out)
        self.assertIn("VERSION", out)

    def test_no_tags_at_all_fails_loudly(self):
        rc, values, out = self.stamp()
        self.assertNotEqual(rc, 0, out)
        self.assertIsNone(values)
        self.assertIn("no v<X>.<Y>.<Z> tag is reachable", out)

    def test_only_a_pre_release_tag_is_no_tag_at_all(self):
        self.tag("v4.0.0-rc.1")
        rc, values, out = self.stamp()
        self.assertNotEqual(rc, 0, out)
        self.assertIn("no v<X>.<Y>.<Z> tag is reachable", out)

    def test_a_pre_release_history_tag_is_never_a_base(self):
        self.tag("v1.2.2")
        self.child()
        self.tag("v4.0.0-rc.1")
        self.child()
        self.assertEqual(self.ok()["kProjectVersion"], f"1.2.3-dev.2+{self.sha()}")

    def test_a_tag_that_is_not_plain_semver_fails_in_the_parser(self):
        for tag in ("v01.2.2", "v1.02.2", "v1.2.02", "v1.2.2+x"):
            with self.subTest(tag=tag):
                git(self.src, "tag", "-a", "-m", tag, tag)
                rc, values, out = self.stamp()
                self.assertNotEqual(rc, 0, out)
                self.assertIn("cannot parse `git describe` output", out)
                git(self.src, "tag", "-d", tag)

    def test_the_stamp_is_rewritten_only_when_its_content_changes(self):
        self.tag("v1.2.2")
        self.child()
        self.ok()
        first = self.out.stat().st_mtime_ns
        time.sleep(0.05)
        self.ok()
        self.assertEqual(self.out.stat().st_mtime_ns, first, "an unchanged stamp must keep its mtime")
        self.child()
        self.ok()
        self.assertNotEqual(self.out.stat().st_mtime_ns, first, "a new commit must rewrite the stamp")


@unittest.skipUnless(shutil.which("cmake") and shutil.which("git"), "cmake and git are required")
class StampParityTest(Scratch):
    """tools/build_version.py is the Python mirror the packaging tools use: it must give the script's version."""

    def test_the_python_mirror_agrees_with_the_script_on_every_shape(self):
        import build_version

        def both(**env):
            v = self.ok(**env)
            mirror = build_version.stamp(self.src, environ=clean_env(**env))
            self.assertEqual(mirror.project_version, v["kProjectVersion"])
            self.assertEqual(mirror.version, v["kVersion"])
            self.assertEqual(mirror.is_release, v["kRelease"])
            self.assertEqual(mirror.describe, v["kDescribe"])

        self.tag("v1.2.2")
        both()
        self.child()
        both()
        self.tag("v1.2.3")
        both(**CI, GITHUB_REF_NAME="v1.2.3")
        self.child()
        both()
        (self.src / "tracked.txt").write_text("edited\n")
        both()


@unittest.skipUnless(shutil.which("cmake") and shutil.which("git") and shutil.which("ninja"), "cmake, git and ninja are required")
class BuildTimeRegenerationTest(unittest.TestCase):
    """A scratch project with the same shape as the tree: a static library holds the one generated unit, the
    refresh target runs the script on every build, and the unit is the only object a new commit recompiles."""

    def setUp(self):
        self.tmp = pathlib.Path(tempfile.mkdtemp(prefix="build-stamp-regen-", dir="/var/tmp"))
        self.addCleanup(shutil.rmtree, self.tmp, ignore_errors=True)
        self.src = self.tmp / "src"
        (self.src / "cmake").mkdir(parents=True)
        shutil.copy(STAMP_CMAKE, self.src / "cmake" / "nevr_build_stamp.cmake")
        shutil.copy(REPO / "cmake" / "nevr_build_stamp_targets.cmake", self.src / "cmake")
        (self.src / "src" / "core").mkdir(parents=True)
        shutil.copy(REPO / "src" / "core" / "build_stamp.h", self.src / "src" / "core")
        (self.src / "VERSION").write_text("1.2.3\n")
        (self.src / "other.cpp").write_text("int other() { return 1; }\n")
        (self.src / "main.cpp").write_text(
            "#include <cstdio>\n#include \"core/build_stamp.h\"\nint other();\n"
            "int main() { std::printf(\"%s %s %d\\n\", nevr_build_stamp::ProjectVersion(), nevr_build_stamp::Describe(), other()); }\n")
        (self.src / "CMakeLists.txt").write_text(
            "cmake_minimum_required(VERSION 3.20)\nproject(toy CXX)\n"
            "find_package(Git QUIET)\n"
            "include(\"${CMAKE_SOURCE_DIR}/cmake/nevr_build_stamp_targets.cmake\")\n"
            "nevr_build_stamp_targets(REPO_DIR \"${CMAKE_SOURCE_DIR}\")\n"
            "add_library(other STATIC other.cpp)\n"
            "add_executable(toy main.cpp)\n"
            "target_link_libraries(toy PRIVATE nevr_build_stamp other)\n")
        git(self.src, "init", "-q", "-b", "main")
        git(self.src, "add", ".")
        git(self.src, "commit", "-q", "-m", "one")
        git(self.src, "tag", "-a", "-m", "v1.2.2", "v1.2.2")
        git(self.src, "commit", "-q", "--allow-empty", "-m", "two")
        self.build = self.tmp / "build"
        subprocess.run(["cmake", "-S", str(self.src), "-B", str(self.build), "-G", "Ninja"],
                       check=True, capture_output=True, text=True, env=clean_env())

    def run_build(self):
        result = subprocess.run(["cmake", "--build", str(self.build)], capture_output=True, text=True,
                                env=clean_env())
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        return result.stdout + result.stderr

    def compiled(self, output):
        return [line for line in output.splitlines() if "Building CXX object" in line]

    def test_a_new_commit_recompiles_only_the_stamp_unit_and_relinks(self):
        first = self.run_build()
        self.assertEqual(len(self.compiled(first)), 3, first)  # other.cpp, main.cpp and the stamp unit
        git(self.src, "commit", "-q", "--allow-empty", "-m", "three")
        second = self.run_build()
        self.assertNotIn("Re-running CMake", second)
        recompiled = self.compiled(second)
        self.assertEqual(len(recompiled), 1, second)
        self.assertIn("nevr_build_stamp.cpp", recompiled[0])
        out = subprocess.run([str(self.build / "toy")], capture_output=True, text=True, check=True).stdout
        head = git(self.src, "rev-parse", "--short", "HEAD").stdout.strip()
        self.assertTrue(out.startswith(f"1.2.3-dev.2+{head} "), out)

    def test_an_edit_to_a_tracked_file_shows_dirty_and_the_revert_is_clean_again(self):
        self.run_build()
        (self.src / "other.cpp").write_text("int other() { return 2; }\n")
        dirty = self.run_build()
        self.assertEqual(len(self.compiled(dirty)), 2, dirty)  # other.cpp and the stamp unit
        out = subprocess.run([str(self.build / "toy")], capture_output=True, text=True, check=True).stdout
        self.assertIn("-dirty ", out)
        git(self.src, "checkout", "--", "other.cpp")
        clean = self.run_build()
        self.assertEqual(len(self.compiled(clean)), 2, clean)
        again = self.run_build()
        self.assertEqual(self.compiled(again), [], again)


if __name__ == "__main__":
    unittest.main()
