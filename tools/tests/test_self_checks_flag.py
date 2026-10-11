"""cmake/nevr_self_checks.cmake: a build reports its run-card checks only when it is stamped a release.

The decision follows the stamped version (cmake/set_project_version_from_git.cmake): a CI build exactly on
the tag vX.Y.Z is stamped X.Y.Z and reports; any other build is stamped X.Y.(Z+1)-dev.<N>+<sha> and does
not, so a local `just package-dev` build never sends debug=true to the game service (#451). The stamp is the
same before and after a release's pre-release flag is unticked, so promotion changes no byte.
"""

import re
import subprocess
import tempfile
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
MODULE = REPO / "cmake" / "nevr_self_checks.cmake"
ROOT = REPO / "CMakeLists.txt"


def by_stamp(version: str) -> str:
    """Runs nevr_self_checks_by_stamp over `version` with `cmake -P` and returns ON or OFF."""
    with tempfile.TemporaryDirectory(prefix="self-checks-flag-") as tmp:
        script = Path(tmp) / "probe.cmake"
        script.write_text(
            f'include("{MODULE.as_posix()}")\n'
            f'nevr_self_checks_by_stamp(RESULT "{version}")\n'
            'message(STATUS "RESULT=${RESULT}")\n',
            encoding="utf-8",
        )
        done = subprocess.run(["cmake", "-P", str(script)], capture_output=True, text=True)
    assert done.returncode == 0, done.stderr
    match = re.search(r"RESULT=(\w+)", done.stdout + done.stderr)
    assert match, done.stdout + done.stderr
    return match.group(1)


class SelfChecksFlagTest(unittest.TestCase):
    def test_a_release_stamp_turns_it_on(self):
        self.assertEqual(by_stamp("5.0.0"), "ON")
        self.assertEqual(by_stamp("4.1.12"), "ON")

    def test_a_development_stamp_or_anything_else_leaves_it_off(self):
        self.assertEqual(by_stamp("5.0.1-dev.1+47841ade"), "OFF")
        self.assertEqual(by_stamp("5.0.1-dev.0+3a35e0b"), "OFF")
        self.assertEqual(by_stamp("5.0.0+3a35e0b"), "OFF")  # a release stamp carries no build metadata
        self.assertEqual(by_stamp("5.0.0-beta.1"), "OFF")
        self.assertEqual(by_stamp("5.0"), "OFF")
        self.assertEqual(by_stamp(""), "OFF")

    def test_the_root_cmake_uses_the_stamped_version_and_nothing_else(self):
        text = ROOT.read_text(encoding="utf-8")
        self.assertIn('nevr_self_checks_by_stamp(NEVR_SELF_CHECKS_BY_STAMP "${PROJECT_VERSION}")', text)
        block = text[text.index("nevr_self_checks_by_stamp(NEVR_SELF_CHECKS_BY_STAMP"):text.index("add_compile_definitions(NEVR_SELF_CHECKS=1)")]
        for forbidden in ("NEVR_RC_LABEL", "ENV{GITHUB", "prerelease"):
            self.assertNotIn(forbidden, block)


if __name__ == "__main__":
    unittest.main()
