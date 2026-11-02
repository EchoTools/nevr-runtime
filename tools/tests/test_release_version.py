"""The version of a build: the committed VERSION file, plus git at build time (#1935).

VERSION (exactly X.Y.Z) is the base of every build. A CI build exactly on the tag vX.Y.Z of VERSION stamps exactly X.Y.Z.
Every other build stamps X.Y.Z-dev.<N>+<sha>, where N is the commits since the nearest release tag, which sorts after
that tag and before VERSION (semver.org items 9-11). Pre-release tags such as the v4.0.0-rc.1 history tag are never a
base. A release is made by a commit that bumps VERSION, and the tag is made from that commit (RELEASING.md).

The REAL code runs: cmake/set_project_version_from_git.cmake (configured by cmake) and cmake/nevr_build_stamp.cmake
(run as the build runs it) in a scratch git repository with real tags.
"""
from __future__ import annotations

import os
import pathlib
import re
import shutil
import subprocess
import tempfile
import unittest

REPO = pathlib.Path(__file__).resolve().parents[2]
VERSION_CMAKE = REPO / "cmake" / "set_project_version_from_git.cmake"
STAMP_CMAKE = REPO / "cmake" / "nevr_build_stamp.cmake"

CMAKELISTS = f"""cmake_minimum_required(VERSION 3.20)
project(toy NONE)
include("{VERSION_CMAKE}")
set_project_version_from_git()
file(WRITE "${{CMAKE_BINARY_DIR}}/version.txt" "${{PROJECT_VERSION}}")
"""

CI = {"GITHUB_ACTIONS": "true", "GITHUB_REF_TYPE": "tag"}


def git(cwd, *args):
    return subprocess.run(["git", "-c", "user.name=t", "-c", "user.email=t@example.invalid",
                           "-c", "commit.gpgsign=false", "-c", "tag.gpgsign=false", *args],
                          cwd=cwd, check=True, capture_output=True, text=True)


# --- semver.org 2.0.0 precedence, items 9-11 -----------------------------------------------------------
# The `semver` PyPI package is not installed here, so the comparison is implemented from the spec and is
# first checked against the spec's own example chain (item 11.4 of semver.org/spec/v2.0.0).
SEMVER = re.compile(r"^(0|[1-9]\d*)\.(0|[1-9]\d*)\.(0|[1-9]\d*)(?:-([0-9A-Za-z.-]+))?(?:\+([0-9A-Za-z.-]+))?$")


def semver_key_cmp(a: str, b: str) -> int:
    """-1/0/1 by semver.org precedence. Item 10: build metadata is ignored. Item 11.2: major, minor, patch
    compared numerically. 11.3: a pre-release version has lower precedence than the normal version.
    11.4: pre-release identifiers compared left to right; numeric ones numerically (11.4.1), alphanumeric
    ones in ASCII order (11.4.2), numeric lower than alphanumeric (11.4.3), a larger set of fields wins
    when all preceding identifiers are equal (11.4.4)."""
    ma, mb = SEMVER.match(a), SEMVER.match(b)
    assert ma and mb, (a, b)
    core_a, core_b = tuple(int(x) for x in ma.groups()[:3]), tuple(int(x) for x in mb.groups()[:3])
    if core_a != core_b:
        return -1 if core_a < core_b else 1
    pre_a, pre_b = ma.group(4), mb.group(4)
    if pre_a is None and pre_b is None:
        return 0
    if pre_a is None:
        return 1
    if pre_b is None:
        return -1
    ids_a, ids_b = pre_a.split("."), pre_b.split(".")
    for x, y in zip(ids_a, ids_b):
        if x == y:
            continue
        xn, yn = x.isdigit(), y.isdigit()
        if xn and yn:
            return -1 if int(x) < int(y) else 1
        if xn != yn:
            return -1 if xn else 1
        return -1 if x < y else 1
    if len(ids_a) == len(ids_b):
        return 0
    return -1 if len(ids_a) < len(ids_b) else 1


class SemverComparatorTest(unittest.TestCase):
    def test_the_spec_example_chain_is_strictly_increasing(self):
        chain = ["1.0.0-alpha", "1.0.0-alpha.1", "1.0.0-alpha.beta", "1.0.0-beta", "1.0.0-beta.2",
                 "1.0.0-beta.11", "1.0.0-rc.1", "1.0.0"]
        for lower, higher in zip(chain, chain[1:]):
            self.assertEqual(semver_key_cmp(lower, higher), -1, (lower, higher))
            self.assertEqual(semver_key_cmp(higher, lower), 1, (higher, lower))

    def test_build_metadata_is_ignored(self):
        self.assertEqual(semver_key_cmp("1.0.0+abc", "1.0.0+def"), 0)
        self.assertEqual(semver_key_cmp("1.0.0-dev.1+zzz", "1.0.0-dev.1+aaa"), 0)

    def test_the_dev_shape_sorts_between_its_tag_and_the_next_release(self):
        self.assertEqual(semver_key_cmp("3.3.0", "3.3.1-dev.3+abc1234"), -1)
        self.assertEqual(semver_key_cmp("3.3.1-dev.3+abc1234", "3.3.1"), -1)
        self.assertEqual(semver_key_cmp("3.3.1-dev.3+abc1234", "3.3.1-dev.10+abc1234"), -1)  # 11.4.1


@unittest.skipUnless(shutil.which("cmake") and shutil.which("git"), "cmake and git are required")
class ReleaseVersionTest(unittest.TestCase):
    def setUp(self):
        self.tmp = pathlib.Path(tempfile.mkdtemp(prefix="release-version-", dir="/var/tmp"))
        self.addCleanup(shutil.rmtree, self.tmp, ignore_errors=True)
        self.src = self.tmp / "src"
        self.src.mkdir()
        (self.src / "CMakeLists.txt").write_text(CMAKELISTS)
        (self.src / "VERSION").write_text("5.0.0\n")
        git(self.src, "init", "-q", "-b", "main")
        git(self.src, "add", "CMakeLists.txt", "VERSION")
        git(self.src, "commit", "-q", "-m", "one")
        self.runs = 0

    def set_version(self, text):
        (self.src / "VERSION").write_text(text)

    def bump(self, text):
        """A release bump is a commit (RELEASING.md): the tree is clean when its tag is made."""
        self.set_version(text)
        git(self.src, "commit", "-q", "-am", f"bump VERSION to {text.strip()}")

    def tag(self, name):
        git(self.src, "tag", "-a", "-m", name, name)

    def child(self):
        git(self.src, "commit", "-q", "--allow-empty", "-m", "child")

    def sha(self):
        return git(self.src, "rev-parse", "--short", "HEAD").stdout.strip()

    def configure(self):
        """(rc, PROJECT_VERSION, output): cmake configures the toy with set_project_version_from_git()."""
        self.runs += 1
        build = self.tmp / f"build{self.runs}"
        clean = {k: v for k, v in os.environ.items() if not k.startswith("GITHUB_")}
        result = subprocess.run(["cmake", "-S", str(self.src), "-B", str(build)],
                                capture_output=True, text=True, env=clean)
        version_file = build / "version.txt"
        version = version_file.read_text() if version_file.exists() else None
        return result.returncode, version, " ".join((result.stdout + result.stderr).split())

    def stamp(self, **env):
        """(rc, kProjectVersion or None, output): the build's stamp, written by nevr_build_stamp.cmake."""
        self.runs += 1
        out = self.tmp / f"stamp{self.runs}" / "nevr_build_stamp.cpp"
        clean = {k: v for k, v in os.environ.items() if not k.startswith("GITHUB_")}
        clean.update(env)
        result = subprocess.run(["cmake", f"-DNEVR_STAMP_SOURCE_DIR={self.src}", f"-DNEVR_STAMP_OUTPUT={out}",
                                 "-P", str(STAMP_CMAKE)], capture_output=True, text=True, env=clean)
        text = " ".join((result.stdout + result.stderr).split())
        match = re.search(r'const char kProjectVersion\[\] = "([^"]*)";', out.read_text()) if out.exists() else None
        return result.returncode, (match.group(1) if match else None), text

    def version(self, **env):
        rc, version, out = self.stamp(**env)
        self.assertEqual(rc, 0, out)
        return version

    def ci(self, tag):
        return dict(CI, GITHUB_REF_NAME=tag)

    # --- the base: PROJECT_VERSION is the VERSION file ------------------------------------------------

    def test_project_version_is_the_version_file_and_git_is_not_read(self):
        self.set_version("5.0.1\n")
        self.tag("v5.0.0")
        self.child()
        rc, version, out = self.configure()
        self.assertEqual(rc, 0, out)
        self.assertEqual(version, "5.0.1")

    def test_a_version_file_with_a_suffix_is_refused_at_configure(self):
        self.set_version("5.0.1-dev.3\n")
        rc, version, out = self.configure()
        self.assertNotEqual(rc, 0, out)
        self.assertIsNone(version)
        self.assertIn("must hold exactly X.Y.Z", out)

    # --- the stamp: a dev build and a release ---------------------------------------------------------

    def test_a_dev_build_is_the_version_file_with_the_distance_and_the_commit(self):
        self.tag("v5.0.0")
        self.set_version("5.0.1\n")
        self.child()
        self.assertEqual(self.version(), f"5.0.1-dev.1+{self.sha()}")

    def test_a_ci_build_exactly_on_the_release_tag_stamps_the_version(self):
        self.bump("5.0.1\n")
        self.tag("v5.0.1")
        self.assertEqual(self.version(**self.ci("v5.0.1")), "5.0.1")

    def test_a_release_tag_on_a_dirty_tree_refuses_to_build(self):
        self.set_version("5.0.1\n")
        self.tag("v5.0.1")
        (self.src / "CMakeLists.txt").write_text(CMAKELISTS + "# edited\n")
        rc, version, out = self.stamp(**self.ci("v5.0.1"))
        self.assertNotEqual(rc, 0, out)
        self.assertIsNone(version)
        self.assertIn("-dirty", out)

    def test_the_wrong_ci_ref_never_stamps_a_release(self):
        self.set_version("5.0.1\n")
        self.tag("v5.0.0")
        self.child()
        dev = f"5.0.1-dev.1+{self.sha()}"
        cases = {
            "branch ref": dict(CI, GITHUB_REF_TYPE="branch", GITHUB_REF_NAME="main"),
            "branch named like the tag": dict(CI, GITHUB_REF_TYPE="branch", GITHUB_REF_NAME="v5.0.1"),
            "no GITHUB_ACTIONS": {"GITHUB_REF_TYPE": "tag", "GITHUB_REF_NAME": "v5.0.1"},
            "GITHUB_ACTIONS not exactly true": dict(CI, GITHUB_ACTIONS="TRUE", GITHUB_REF_NAME="v5.0.1"),
            "another tag": dict(CI, GITHUB_REF_NAME="v5.0.0"),
            "refs/tags prefix": dict(CI, GITHUB_REF_NAME="refs/tags/v5.0.1"),
        }
        for name, env in cases.items():
            with self.subTest(name):
                self.assertEqual(self.version(**env), dev)

    # --- history tags and the single source ----------------------------------------------------------

    def test_a_pre_release_history_tag_is_never_a_base(self):
        self.tag("v3.3.0")
        self.child()
        self.tag("v4.0.0-rc.1")
        self.child()
        self.set_version("3.3.1\n")
        self.assertEqual(self.version(), f"3.3.1-dev.2+{self.sha()}")  # v4.0.0-rc.1 is ignored

    def test_before_the_first_new_tag_a_dev_build_takes_the_nearest_release_tag(self):
        self.tag("v3.3.0")
        self.child()
        self.child()
        self.set_version("3.3.1\n")
        self.assertEqual(self.version(), f"3.3.1-dev.2+{self.sha()}")

    def test_no_tags_at_all_fails_loudly(self):
        rc, version, out = self.stamp()
        self.assertNotEqual(rc, 0)
        self.assertIsNone(version)
        self.assertIn("no v<X>.<Y>.<Z> tag is reachable", out)

    def test_only_a_pre_release_tag_is_no_tag_at_all(self):
        self.tag("v4.0.0-rc.1")
        rc, version, out = self.stamp()
        self.assertNotEqual(rc, 0)
        self.assertIn("no v<X>.<Y>.<Z> tag is reachable", out)

    def test_a_tag_that_is_not_plain_semver_fails_in_the_parser_not_with_a_half_version(self):
        """`vnext` is filtered out by --match "v[0-9]*" and exercises the no-tag branch (above). These pass
        the glob, so describe returns them and the PARSE-failure branch is the one that must stop the
        build: leading zeros and build metadata are not release versions."""
        for tag in ("v05.0.0", "v5.00.0", "v5.0.00", "v5.0.0+x"):
            with self.subTest(tag=tag):
                git(self.src, "tag", "-a", "-m", tag, tag)
                rc, version, out = self.stamp()
                self.assertNotEqual(rc, 0, out)
                self.assertIsNone(version)
                self.assertIn("cannot parse `git describe` output", out)
                self.assertIn(tag, out)
                git(self.src, "tag", "-d", tag)

    # --- precedence, with the real comparator --------------------------------------------------------

    def test_every_non_tag_build_sorts_after_its_tag_and_before_the_next_release(self):
        self.tag("v3.3.0")
        self.set_version("3.3.1\n")
        base = "3.3.0"
        self.child()
        first = self.version()
        self.child()
        second = self.version()
        self.assertEqual(semver_key_cmp(base, first), -1)
        self.assertEqual(semver_key_cmp(first, second), -1)
        self.assertEqual(semver_key_cmp(second, "3.3.1"), -1)
        self.assertEqual(semver_key_cmp(second, "3.4.0"), -1)

    def test_a_release_sorts_above_the_dev_builds_before_it_and_below_the_next_version(self):
        self.tag("v5.0.0")
        self.bump("5.0.1\n")
        self.child()
        before = self.version()
        self.tag("v5.0.1")
        released = self.version(**self.ci("v5.0.1"))
        self.bump("5.0.2\n")  # the next development starts with the next version (RELEASING.md)
        self.child()
        after = self.version()
        self.assertEqual(after, f"5.0.2-dev.2+{self.sha()}")
        self.assertEqual(released, "5.0.1")
        self.assertEqual(semver_key_cmp(before, released), -1)
        self.assertEqual(semver_key_cmp(released, after), -1)


class LocalRecipeTest(unittest.TestCase):
    """`just package-dev` is the only local package recipe; it asks cmake for nothing special."""

    def recipe(self, name: str) -> str:
        text = (REPO / "justfile").read_text()
        start = text.index(f"\n{name} ")
        return text[start:text.index("\n\n", start)]

    def test_the_local_recipe_passes_no_version_or_label_and_runs_the_preflight_first(self):
        body = self.recipe("package-dev")
        self.assertNotIn("NEVR_RC_LABEL", body)
        self.assertNotIn("--version", body)
        self.assertIn("tools/package_release.py --commit", body)
        self.assertNotIn("package_rc", body)

    def test_no_retired_recipe_or_tool_remains(self):
        justfile = (REPO / "justfile").read_text()
        self.assertNotIn("\npackage-rc ", justfile)
        for retired in ("cmake/nevr_rc_label.cmake", "tools/package_rc.py", "tools/rc_release_apk.sh",
                        "tools/package-rc"):
            self.assertFalse((REPO / retired).exists(), retired)


if __name__ == "__main__":
    unittest.main()
