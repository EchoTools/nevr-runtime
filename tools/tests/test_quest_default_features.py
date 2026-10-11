"""cmake/nevr_build_info.cmake: NEVR_QUEST_DEFAULT_FEATURES names only features the sentinel has (#451).

`self_check` was a Quest feature and is gone (self-checks are on in every build): a build that still passes it in
NEVR_QUEST_DEFAULT_FEATURES must fail the configure loudly rather than be silently ignored.
"""

import subprocess
import tempfile
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
MODULE = REPO / "cmake" / "nevr_build_info.cmake"


def validate(features: str) -> subprocess.CompletedProcess:
    with tempfile.TemporaryDirectory(prefix="quest-default-features-") as tmp:
        script = Path(tmp) / "probe.cmake"
        script.write_text(
            f'include("{MODULE.as_posix()}")\n'
            f'nevr_validate_default_features("{features}")\n'
            'message(STATUS "VALID")\n',
            encoding="utf-8",
        )
        return subprocess.run(["cmake", "-P", str(script)], capture_output=True, text=True)


class QuestDefaultFeaturesTest(unittest.TestCase):
    def test_the_features_the_sentinel_has_are_accepted(self):
        for features in ("", "redirect,bridge,login,social", "hwdump,obb_skip"):
            done = validate(features)
            self.assertEqual(done.returncode, 0, done.stderr)
            self.assertIn("VALID", done.stdout + done.stderr)

    def test_the_retired_self_check_feature_fails_the_configure(self):
        done = validate("redirect,bridge,login,social,self_check")
        self.assertNotEqual(done.returncode, 0)
        self.assertIn("unknown feature 'self_check'", done.stderr)

    def test_any_unknown_name_fails_the_configure(self):
        done = validate("redirect,nothing_like_it")
        self.assertNotEqual(done.returncode, 0)
        self.assertIn("unknown feature 'nothing_like_it'", done.stderr)


if __name__ == "__main__":
    unittest.main()
