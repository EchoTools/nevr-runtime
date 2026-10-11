"""Only a CI build on a v<x.y.z>-rc.<N> tag may stamp -rc.<N> into a version string.

cmake/nevr_rc_label.cmake is the one place a label becomes a version (the DLL and the Quest sentinel
both go through nevr_apply_rc_label). Anything else stamps <x.y.z>-dev+<tweak>.<sha> and says so.
"""
from __future__ import annotations

import os
import pathlib
import subprocess
import tempfile
import unittest

REPO = pathlib.Path(__file__).resolve().parents[2]
MODULE = REPO / "cmake" / "nevr_rc_label.cmake"
BASE = "4.0.0+1199.abc1234"

SCRIPT = f"""
set(PROJECT_VERSION "{BASE}")
set(GIT_COMMIT_HASH "abc1234")
include("{MODULE}")
nevr_apply_rc_label()
message("STAMPED=${{PROJECT_VERSION}}")
"""


def stamp(label: str, **env: str) -> subprocess.CompletedProcess:
    clean = {k: v for k, v in os.environ.items() if not k.startswith("GITHUB_")}
    clean.update(env)
    with tempfile.TemporaryDirectory(prefix="rc-stamp-") as tmp:
        script = pathlib.Path(tmp) / "stamp.cmake"
        script.write_text(SCRIPT)
        args = ["cmake"]
        if label:
            args.append(f"-DNEVR_RC_LABEL={label}")
        args += ["-P", str(script)]
        return subprocess.run(args, capture_output=True, text=True, env=clean, cwd=tmp)


def stamped(result: subprocess.CompletedProcess) -> str:
    text = result.stdout + result.stderr
    for line in text.splitlines():
        if line.startswith("STAMPED="):
            return line[len("STAMPED="):]
    raise AssertionError(f"no STAMPED line (exit {result.returncode}):\n{text}")


CI_TAG = {"GITHUB_ACTIONS": "true", "GITHUB_REF_TYPE": "tag"}


class RcStampTest(unittest.TestCase):
    def test_a_local_build_with_a_label_stamps_dev_and_says_so(self):
        result = stamp("rc.1")
        self.assertEqual(stamped(result), "4.0.0-dev+1199.abc1234")
        self.assertIn("rc.1 ignored", result.stdout + result.stderr)

    def test_a_ci_build_on_a_branch_stamps_dev(self):
        env = {"GITHUB_ACTIONS": "true", "GITHUB_REF_TYPE": "branch", "GITHUB_REF_NAME": "main"}
        self.assertEqual(stamped(stamp("rc.1", **env)), "4.0.0-dev+1199.abc1234")

    def test_a_branch_that_is_named_like_an_rc_tag_still_stamps_dev(self):
        # The ref NAME alone proves nothing: only GITHUB_REF_TYPE says whether it is a tag.
        env = {"GITHUB_ACTIONS": "true", "GITHUB_REF_TYPE": "branch", "GITHUB_REF_NAME": "v4.0.0-rc.1"}
        self.assertEqual(stamped(stamp("rc.1", **env)), "4.0.0-dev+1199.abc1234")

    def test_a_tag_that_is_not_a_release_candidate_tag_stamps_dev(self):
        for name in ("v4.0.0", "v4.0.0-beta.1", "rc.1", "v4.0.0-rc.", "v4.0.0-rc.1x", "xv4.0.0-rc.1"):
            with self.subTest(tag=name):
                env = dict(CI_TAG, GITHUB_REF_NAME=name)
                self.assertEqual(stamped(stamp("rc.1", **env)), "4.0.0-dev+1199.abc1234")

    def test_a_tag_whose_number_differs_from_the_label_stamps_dev(self):
        env = dict(CI_TAG, GITHUB_REF_NAME="v4.0.0-rc.2")
        self.assertEqual(stamped(stamp("rc.1", **env)), "4.0.0-dev+1199.abc1234")

    def test_a_ci_job_flag_without_actions_does_not_count(self):
        env = {"GITHUB_REF_TYPE": "tag", "GITHUB_REF_NAME": "v4.0.0-rc.1"}  # GITHUB_ACTIONS missing
        self.assertEqual(stamped(stamp("rc.1", **env)), "4.0.0-dev+1199.abc1234")

    def test_a_ci_build_on_the_matching_rc_tag_stamps_rc(self):
        env = dict(CI_TAG, GITHUB_REF_NAME="v4.0.0-rc.1")
        self.assertEqual(stamped(stamp("rc.1", **env)), "4.0.0-rc.1+1199.abc1234")
        env = dict(CI_TAG, GITHUB_REF_NAME="v4.1.0-rc.12")
        self.assertEqual(stamped(stamp("rc.12", **env)), "4.0.0-rc.12+1199.abc1234")

    def test_no_label_leaves_the_version_untouched_everywhere(self):
        self.assertEqual(stamped(stamp("")), BASE)
        env = dict(CI_TAG, GITHUB_REF_NAME="v4.0.0-rc.1")
        self.assertEqual(stamped(stamp("", **env)), BASE)

    def test_the_dev_label_stamps_dev_everywhere_even_on_an_rc_tag(self):
        self.assertEqual(stamped(stamp("dev")), "4.0.0-dev+1199.abc1234")
        env = dict(CI_TAG, GITHUB_REF_NAME="v4.0.0-rc.1")
        result = stamp("dev", **env)
        self.assertEqual(stamped(result), "4.0.0-dev+1199.abc1234")
        self.assertIn("not a release candidate", result.stdout + result.stderr)

    def test_a_malformed_label_is_still_refused(self):
        result = stamp("rc.x")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("must be dev or look like rc.<N>", result.stdout + result.stderr)


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
