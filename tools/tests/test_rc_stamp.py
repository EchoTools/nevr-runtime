"""Only a CI build exactly on the tag v<x.y.z>-rc.<N> may stamp -rc.<N> into a version string.

The REAL path runs here: a scratch git repository with real tags, configured by cmake with
cmake/set_project_version_from_git.cmake (X.Y.Z comes from the nearest v* tag, an rc tag counting as its
base) followed by nevr_apply_rc_label (cmake/nevr_rc_label.cmake, the one place a label becomes a version;
the DLL and the Quest sentinel both call it). Every other build stamps <x.y.z>-dev+<distance>.<sha>.
"""
from __future__ import annotations

import os
import pathlib
import shutil
import subprocess
import tempfile
import unittest

REPO = pathlib.Path(__file__).resolve().parents[2]
VERSION_CMAKE = REPO / "cmake" / "set_project_version_from_git.cmake"
LABEL_CMAKE = REPO / "cmake" / "nevr_rc_label.cmake"

CMAKELISTS = f"""cmake_minimum_required(VERSION 3.20)
project(toy NONE)
include("{VERSION_CMAKE}")
include("{LABEL_CMAKE}")
set_project_version_from_git()
nevr_apply_rc_label()
file(WRITE "${{CMAKE_BINARY_DIR}}/version.txt" "${{PROJECT_VERSION}}")
"""

CI = {"GITHUB_ACTIONS": "true", "GITHUB_REF_TYPE": "tag"}


def git(cwd, *args):
    return subprocess.run(["git", "-c", "user.name=t", "-c", "user.email=t@example.invalid",
                           "-c", "commit.gpgsign=false", "-c", "tag.gpgsign=false", *args],
                          cwd=cwd, check=True, capture_output=True, text=True)


@unittest.skipUnless(shutil.which("cmake") and shutil.which("git"), "cmake and git are required")
class RcStampTest(unittest.TestCase):
    def setUp(self):
        self.tmp = pathlib.Path(tempfile.mkdtemp(prefix="rc-stamp-", dir="/var/tmp"))
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

    def configure(self, label="", **env):
        """cmake configure of the scratch repo at HEAD; returns (exit code, version or None, output)."""
        self.runs += 1
        build = self.tmp / f"build{self.runs}"
        clean = {k: v for k, v in os.environ.items() if not k.startswith("GITHUB_")}
        clean.update(env)
        args = ["cmake", "-S", str(self.src), "-B", str(build)]
        if label:
            args.append(f"-DNEVR_RC_LABEL={label}")
        result = subprocess.run(args, capture_output=True, text=True, env=clean)
        version_file = build / "version.txt"
        version = version_file.read_text() if version_file.exists() else None
        # CMake wraps long error text; compare it as one line.
        return result.returncode, version, " ".join((result.stdout + result.stderr).split())

    def version(self, label="", **env):
        rc, version, out = self.configure(label, **env)
        self.assertEqual(rc, 0, out)
        return version

    # --- the CI build exactly on the rc tag ---------------------------------------------------------

    def test_a_ci_build_exactly_on_the_rc_tag_stamps_rc(self):
        self.tag("v4.0.0-rc.1")
        env = dict(CI, GITHUB_REF_NAME="v4.0.0-rc.1")
        self.assertEqual(self.version("rc.1", **env), f"4.0.0-rc.1+0.{self.sha()}")

    def test_the_version_base_is_the_tags_own_not_a_fixed_4_0_0(self):
        self.tag("v4.1.0-rc.12")
        env = dict(CI, GITHUB_REF_NAME="v4.1.0-rc.12")
        self.assertEqual(self.version("rc.12", **env), f"4.1.0-rc.12+0.{self.sha()}")

    # --- everything else is a development version ---------------------------------------------------

    def test_a_child_of_the_rc_tag_stamps_dev_even_with_the_ci_env_and_the_label(self):
        self.tag("v4.0.0-rc.1")
        self.child()
        env = dict(CI, GITHUB_REF_NAME="v4.0.0-rc.1")
        self.assertEqual(self.version("rc.1", **env), f"4.0.0-dev+1.{self.sha()}")
        self.assertEqual(self.version("", **env), f"4.0.0-dev+1.{self.sha()}")
        self.child()
        self.assertEqual(self.version(), f"4.0.0-dev+2.{self.sha()}")

    def test_the_rc_tag_built_locally_stamps_dev_and_says_so(self):
        self.tag("v4.0.0-rc.1")
        rc, version, out = self.configure("rc.1")
        self.assertEqual((rc, version), (0, f"4.0.0-dev+0.{self.sha()}"))
        self.assertIn("rc.1 ignored", out)
        self.assertEqual(self.version(), f"4.0.0-dev+0.{self.sha()}")  # no label: still dev, never -rc

    def test_the_rc_tag_in_ci_on_the_wrong_ref_or_number_stamps_dev(self):
        self.tag("v4.0.0-rc.1")
        dev = f"4.0.0-dev+0.{self.sha()}"
        cases = {
            "branch ref": dict(CI, GITHUB_REF_TYPE="branch", GITHUB_REF_NAME="main"),
            "branch named like the tag": dict(CI, GITHUB_REF_TYPE="branch", GITHUB_REF_NAME="v4.0.0-rc.1"),
            "no GITHUB_ACTIONS": {"GITHUB_REF_TYPE": "tag", "GITHUB_REF_NAME": "v4.0.0-rc.1"},
            "GITHUB_ACTIONS not exactly true": dict(CI, GITHUB_ACTIONS="TRUE", GITHUB_REF_NAME="v4.0.0-rc.1"),
            "a plain release tag ref": dict(CI, GITHUB_REF_NAME="v4.0.0"),
            "ref number differs": dict(CI, GITHUB_REF_NAME="v4.0.0-rc.2"),
            "ref with refs/tags prefix": dict(CI, GITHUB_REF_NAME="refs/tags/v4.0.0-rc.1"),
        }
        for name, env in cases.items():
            with self.subTest(name):
                self.assertEqual(self.version("rc.1", **env), dev)

    def test_the_label_number_must_be_the_tags_number(self):
        self.tag("v4.0.0-rc.1")
        env = dict(CI, GITHUB_REF_NAME="v4.0.0-rc.2")  # a CI tag ref for rc.2, the checkout is rc.1
        self.assertEqual(self.version("rc.2", **env), f"4.0.0-dev+0.{self.sha()}")

    def test_the_dev_label_stamps_dev_even_on_the_rc_tag_in_ci(self):
        self.tag("v4.0.0-rc.1")
        env = dict(CI, GITHUB_REF_NAME="v4.0.0-rc.1")
        rc, version, out = self.configure("dev", **env)
        self.assertEqual((rc, version), (0, f"4.0.0-dev+0.{self.sha()}"))
        self.assertIn("not a release candidate", out)

    def test_a_malformed_label_is_refused(self):
        self.tag("v4.0.0-rc.1")
        rc, version, out = self.configure("rc.x")
        self.assertNotEqual(rc, 0)
        self.assertIn("must be dev or look like rc.<N>", out)

    # --- tags are the single source -----------------------------------------------------------------

    def test_a_plain_release_tag_is_the_release_version_and_its_children_are_dev(self):
        self.tag("v3.3.0")
        self.assertEqual(self.version(), f"3.3.0+0.{self.sha()}")
        self.child()
        self.assertEqual(self.version(), f"3.3.0-dev+1.{self.sha()}")

    def test_before_the_rc_tag_exists_a_dev_build_takes_the_nearest_older_tag(self):
        self.tag("v3.3.0")
        self.child()
        self.child()
        self.assertEqual(self.version(), f"3.3.0-dev+2.{self.sha()}")  # what main stamps until the rc tag lands

    def test_an_rc_tag_is_its_own_base_when_it_is_the_nearest_tag(self):
        self.tag("v3.3.0")
        self.child()
        self.tag("v4.0.0-rc.1")
        self.child()
        self.assertEqual(self.version(), f"4.0.0-dev+1.{self.sha()}")

    def test_a_plain_and_an_rc_tag_on_one_commit_both_parse(self):
        self.tag("v4.0.0-rc.1")
        self.tag("v4.0.0")
        self.assertTrue(self.version().startswith("4.0.0"))

    def test_no_tags_at_all_fails_loudly(self):
        rc, version, out = self.configure()
        self.assertNotEqual(rc, 0)
        self.assertIsNone(version)
        self.assertIn("no v<X>.<Y>.<Z> or v<X>.<Y>.<Z>-rc.<N> tag is reachable", out)

    def test_a_tag_that_does_not_parse_fails_loudly_not_with_a_half_version(self):
        self.tag("vnext")
        rc, version, out = self.configure()
        self.assertNotEqual(rc, 0)
        self.assertIsNone(version)
        self.assertIn("cannot parse `git describe` output", out)


class LocalRecipeTest(unittest.TestCase):
    """`just package-dev` is the only local package recipe and it can only ask for a dev stamp."""

    def recipe(self, name: str) -> str:
        text = (REPO / "justfile").read_text()
        start = text.index(f"\n{name} ")
        end = text.index("\n\n", start)
        return text[start:end]

    def test_the_local_recipe_asks_for_dev_never_for_an_rc_label(self):
        body = self.recipe("package-dev")
        self.assertEqual(body.count("-DNEVR_RC_LABEL=dev"), 2)  # the DLL and the Quest sentinel
        self.assertNotIn("-DNEVR_RC_LABEL=rc", body)
        self.assertNotIn('label="rc.', body)
        self.assertNotIn("--n ", body)
        self.assertNotIn("\npackage-rc ", (REPO / "justfile").read_text())

    def test_presets_and_docs_name_no_local_rc_recipe(self):
        for relative in ("AGENTS.md", "README.md", "CMakePresets.json", "src/quest/CMakePresets.json", "justfile"):
            text = (REPO / relative).read_text()
            self.assertNotIn("just package-rc", text, relative)


if __name__ == "__main__":
    unittest.main()
