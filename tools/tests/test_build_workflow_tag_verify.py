"""The Verify step of build.yml skips tag builds and runs on every other build (#1008).

Every commit on main already passed `just verify` on its PR, so a tag build repeating the test suite only
adds a timeout risk (the v5.7.0 release build). The condition is `github.ref_type != 'tag'`: a release
event and a dispatch on a vX.Y.Z tag both carry the tag ref. Branch and main dispatches keep the step.
"""

import pathlib
import re
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[2]
BUILD = ROOT / ".github" / "workflows" / "build.yml"


def verify_step(text: str) -> str:
    """The text of the `- name: Verify` step, from its name to the `run: just verify` line."""
    match = re.search(r"^    - name: Verify\n(.*?)^      run: just verify$", text, re.S | re.M)
    if match is None:
        raise AssertionError("build.yml has no `- name: Verify` step running `just verify`")
    return match.group(0)


class TagBuildsSkipVerify(unittest.TestCase):
    def setUp(self) -> None:
        self.text = BUILD.read_text(encoding="utf-8")

    def test_verify_step_is_skipped_for_tag_builds(self) -> None:
        step = verify_step(self.text)
        self.assertIn("if: github.ref_type != 'tag'", step)

    def test_verify_step_still_runs_just_verify(self) -> None:
        self.assertIn("run: just verify", verify_step(self.text))

    def test_the_condition_is_on_the_verify_step_itself(self) -> None:
        # The `if:` must sit between the step name and its run line, so it gates this step and no other.
        step = verify_step(self.text)
        self.assertEqual(step.count("if: "), 1, step)
        self.assertLess(step.index("if: github.ref_type"), step.index("run: just verify"))

    def test_ndk_step_has_the_condition_of_verify(self) -> None:
        # Verify is the only step of the build job that needs the NDK, so the step that sets it up carries Verify's
        # condition, and sits between the build and Verify (#1770). The quest job has its own NDK step, later in the file.
        match = re.search(r"^    - name: Set up the Android NDK\n(.*?)^      uses: \./\.github/actions/setup-ndk\n",
                          self.text, re.S | re.M)
        self.assertIsNotNone(match, "build.yml has no `Set up the Android NDK` step in the build job")
        self.assertIn("if: github.ref_type != 'tag'", match.group(0))
        build = self.text.index("\n    - name: Build\n")
        verify = self.text.index("\n    - name: Verify\n")
        self.assertLess(build, match.start())
        self.assertLess(match.start(), verify)


if __name__ == "__main__":
    unittest.main()
