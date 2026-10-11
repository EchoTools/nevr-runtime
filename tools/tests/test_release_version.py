"""The version of a build comes from git tags only: plain semver.

A CI build exactly on the tag vX.Y.Z stamps exactly X.Y.Z. Every other build stamps
X.Y.(Z+1)-dev.<N>+<sha>, which sorts after the tag it descends from and before the next release
(semver.org items 9-11). Pre-release tags such as the v4.0.0-rc.1 history tag are never a base.

The REAL function runs: cmake/set_project_version_from_git.cmake in a scratch git repository with real
tags, configured by cmake (not a hand-set PROJECT_VERSION).
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
        self.addCleanup(shutil.rmtree, self.tmp)
        self.src = self.tmp / "src"
        self.src.mkdir()
        (self.src / "CMakeLists.txt").write_text(CMAKELISTS)
        git(self.src, "init", "-q", "-b", "main")
        git(self.src, "add", "CMakeLists.txt")
        git(self.src, "commit", "-q", "-m", "one")
        self.runs = 0

    def tag(self, name):
        git(self.src, "tag", "-a", "-m", name, name)

    def child(self):
        git(self.src, "commit", "-q", "--allow-empty", "-m", "child")

    def sha(self):
        return git(self.src, "rev-parse", "--short", "HEAD").stdout.strip()

    def configure(self, **env):
        self.runs += 1
        build = self.tmp / f"build{self.runs}"
        clean = {k: v for k, v in os.environ.items() if not k.startswith("GITHUB_")}
        clean.update(env)
        result = subprocess.run(["cmake", "-S", str(self.src), "-B", str(build)],
                                capture_output=True, text=True, env=clean)
        version_file = build / "version.txt"
        version = version_file.read_text() if version_file.exists() else None
        return result.returncode, version, " ".join((result.stdout + result.stderr).split())

    def version(self, **env):
        rc, version, out = self.configure(**env)
        self.assertEqual(rc, 0, out)
        return version

    def ci(self, tag):
        return dict(CI, GITHUB_REF_NAME=tag)

    # --- a release: exactly the tag ------------------------------------------------------------------

    def test_a_ci_build_exactly_on_a_release_tag_stamps_exactly_the_tag(self):
        self.tag("v5.0.0")
        self.assertEqual(self.version(**self.ci("v5.0.0")), "5.0.0")

    def test_a_child_of_the_tag_is_the_next_patch_dev_even_with_the_ci_env(self):
        self.tag("v5.0.0")
        self.child()
        self.assertEqual(self.version(**self.ci("v5.0.0")), f"5.0.1-dev.1+{self.sha()}")
        self.assertEqual(self.version(), f"5.0.1-dev.1+{self.sha()}")
        self.child()
        self.assertEqual(self.version(), f"5.0.1-dev.2+{self.sha()}")

    def test_the_next_tag_is_the_new_base(self):
        self.tag("v5.0.0")
        self.child()
        self.tag("v5.0.1")
        self.assertEqual(self.version(**self.ci("v5.0.1")), "5.0.1")
        self.child()
        self.assertEqual(self.version(), f"5.0.2-dev.1+{self.sha()}")

    def test_the_tag_built_outside_ci_is_a_dev_build(self):
        self.tag("v5.0.0")
        self.assertEqual(self.version(), f"5.0.1-dev.0+{self.sha()}")

    def test_the_wrong_ci_ref_never_stamps_a_release(self):
        self.tag("v5.0.0")
        dev = f"5.0.1-dev.0+{self.sha()}"
        cases = {
            "branch ref": dict(CI, GITHUB_REF_TYPE="branch", GITHUB_REF_NAME="main"),
            "branch named like the tag": dict(CI, GITHUB_REF_TYPE="branch", GITHUB_REF_NAME="v5.0.0"),
            "no GITHUB_ACTIONS": {"GITHUB_REF_TYPE": "tag", "GITHUB_REF_NAME": "v5.0.0"},
            "GITHUB_ACTIONS not exactly true": dict(CI, GITHUB_ACTIONS="TRUE", GITHUB_REF_NAME="v5.0.0"),
            "another tag": dict(CI, GITHUB_REF_NAME="v5.0.1"),
            "refs/tags prefix": dict(CI, GITHUB_REF_NAME="refs/tags/v5.0.0"),
        }
        for name, env in cases.items():
            with self.subTest(name):
                self.assertEqual(self.version(**env), dev)

    # --- history tags and the single source ---------------------------------------------------------

    def test_a_pre_release_history_tag_is_never_a_base(self):
        self.tag("v3.3.0")
        self.child()
        self.tag("v4.0.0-rc.1")
        self.child()
        self.assertEqual(self.version(), f"3.3.1-dev.2+{self.sha()}")  # v4.0.0-rc.1 is ignored

    def test_before_the_first_new_tag_a_dev_build_takes_the_nearest_release_tag(self):
        self.tag("v3.3.0")
        self.child()
        self.child()
        self.assertEqual(self.version(), f"3.3.1-dev.2+{self.sha()}")

    def test_no_tags_at_all_fails_loudly(self):
        rc, version, out = self.configure()
        self.assertNotEqual(rc, 0)
        self.assertIsNone(version)
        self.assertIn("no v<X>.<Y>.<Z> tag is reachable", out)

    def test_only_a_pre_release_tag_is_no_tag_at_all(self):
        self.tag("v4.0.0-rc.1")
        rc, version, out = self.configure()
        self.assertNotEqual(rc, 0)
        self.assertIn("no v<X>.<Y>.<Z> tag is reachable", out)

    def test_a_tag_that_does_not_parse_fails_loudly_not_with_a_half_version(self):
        self.tag("vnext")
        rc, version, out = self.configure()
        self.assertNotEqual(rc, 0)
        self.assertIsNone(version)

    # --- precedence, with the real comparator --------------------------------------------------------

    def test_every_non_tag_build_sorts_after_its_tag_and_before_the_next_release(self):
        self.tag("v3.3.0")
        base = "3.3.0"
        self.child()
        first = self.version()
        self.child()
        second = self.version()
        self.assertEqual(semver_key_cmp(base, first), -1)
        self.assertEqual(semver_key_cmp(first, second), -1)
        self.assertEqual(semver_key_cmp(second, "3.3.1"), -1)
        self.assertEqual(semver_key_cmp(second, "3.4.0"), -1)

    def test_a_release_sorts_above_the_dev_builds_before_it_and_below_those_after_it(self):
        self.tag("v5.0.0")
        self.child()
        before = self.version()
        self.tag("v5.0.1")
        released = self.version(**self.ci("v5.0.1"))
        self.child()
        after = self.version()
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
