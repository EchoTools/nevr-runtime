"""cmake/nevr_self_checks.cmake: a build reports its run-card checks only when it is stamped a release candidate.

The decision follows the stamped version, not the raw NEVR_RC_LABEL: `just package-dev` passes
-DNEVR_RC_LABEL=dev (stamped -dev) and a local -DNEVR_RC_LABEL=rc.<N> is stamped -dev too (#459), so
neither may send debug=true to the game service (#451).
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
    def test_a_release_candidate_stamp_turns_it_on(self):
        self.assertEqual(by_stamp("4.0.0-rc.3+0.3a35e0b9"), "ON")
        self.assertEqual(by_stamp("4.1.0-rc.12+0.deadbeef"), "ON")

    def test_a_development_or_plain_stamp_leaves_it_off(self):
        # NEVR_RC_LABEL=dev (just package-dev) and a label that was not honoured both stamp -dev.
        self.assertEqual(by_stamp("4.0.0-dev+1205.47841ade"), "OFF")
        self.assertEqual(by_stamp("4.0.0-dev+0.3a35e0b9"), "OFF")
        self.assertEqual(by_stamp("4.0.0+1205.47841ade"), "OFF")
        self.assertEqual(by_stamp(""), "OFF")

    def test_the_root_cmake_uses_the_stamped_version_and_not_the_raw_label(self):
        text = ROOT.read_text(encoding="utf-8")
        self.assertIn('nevr_self_checks_by_stamp(NEVR_SELF_CHECKS_BY_STAMP "${PROJECT_VERSION}")', text)
        # the block that defines NEVR_SELF_CHECKS must not test NEVR_RC_LABEL
        block = text[text.index("nevr_self_checks_by_stamp(NEVR_SELF_CHECKS_BY_STAMP"):text.index("add_compile_definitions(NEVR_SELF_CHECKS=1)")]
        self.assertNotIn("NEVR_RC_LABEL", block)


if __name__ == "__main__":
    unittest.main()
