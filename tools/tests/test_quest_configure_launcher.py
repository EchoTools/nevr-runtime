"""The Quest configure recipes pass the compiler launcher as -D values (#1976).

CMake reads CMAKE_*_COMPILER_LAUNCHER from the environment only on a build tree's first configure, so an existing
android tree (build/android-arm64, build/android-dev) keeps whatever launcher it was first configured with: the
android-dev tree on a maintainer machine had none. Each configure of an android preset therefore names the launcher
with -D, and a re-configure applies it.

The check reads the justfile text. It does not configure anything.
"""

from __future__ import annotations

import pathlib
import re
import unittest

REPO = pathlib.Path(__file__).resolve().parents[2]
JUSTFILE = REPO / "justfile"
ANDROID_CONFIGURE = re.compile(r"cmake --preset android-[a-z0-9-]+")
C_LAUNCHER = "-DCMAKE_C_COMPILER_LAUNCHER=\"$launcher\""
CXX_LAUNCHER = "-DCMAKE_CXX_COMPILER_LAUNCHER=\"$launcher\""


class QuestConfigureLauncher(unittest.TestCase):
    def test_every_android_configure_passes_the_launcher_as_d_values(self) -> None:
        lines = [line.strip() for line in JUSTFILE.read_text(encoding="utf-8").splitlines() if ANDROID_CONFIGURE.search(line)]
        self.assertTrue(lines, "the justfile has no android configure line to check")
        for line in lines:
            with self.subTest(line=line):
                self.assertIn(C_LAUNCHER, line, "the C launcher is not passed with -D")
                self.assertIn(CXX_LAUNCHER, line, "the C++ launcher is not passed with -D")

    def test_the_environment_is_not_the_only_carrier_of_the_launcher(self) -> None:
        # The launcher variables are still read from the environment (the recipe's `launcher=` line); -D carries them to CMake.
        text = JUSTFILE.read_text(encoding="utf-8")
        self.assertIn('launcher="${CMAKE_CXX_COMPILER_LAUNCHER:-}"', text)


if __name__ == "__main__":
    unittest.main()
