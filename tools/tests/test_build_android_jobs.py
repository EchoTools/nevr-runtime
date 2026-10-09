"""`just build-android` bounds its job count: CMAKE_BUILD_PARALLEL_LEVEL if set, else 4, never a bare -j."""
from __future__ import annotations

import os
import pathlib
import shutil
import stat
import subprocess
import tempfile
import unittest

REPO = pathlib.Path(__file__).resolve().parents[2]


@unittest.skipUnless(shutil.which("just"), "just is required")
class BuildAndroidJobsTest(unittest.TestCase):
    def run_recipe(self, parallel_level):
        with tempfile.TemporaryDirectory(prefix="build-android-jobs-", dir="/var/tmp") as tmp:
            tmp = pathlib.Path(tmp)
            log = tmp / "cmake-calls.txt"
            fake = tmp / "bin" / "cmake"
            fake.parent.mkdir()
            fake.write_text(f'#!/bin/sh\necho "$@" >> {log}\n')
            fake.chmod(fake.stat().st_mode | stat.S_IEXEC)
            env = dict(os.environ, PATH=f"{fake.parent}:{os.environ['PATH']}", ANDROID_NDK_HOME="/nonexistent-ndk")
            env.pop("CMAKE_BUILD_PARALLEL_LEVEL", None)
            if parallel_level is not None:
                env["CMAKE_BUILD_PARALLEL_LEVEL"] = parallel_level
            result = subprocess.run(["just", "build-android"], cwd=REPO, env=env, capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            return log.read_text().splitlines()

    def test_the_variable_sets_the_job_count(self):
        calls = self.run_recipe("3")
        self.assertIn("--build build/android-arm64 -j 3", calls)

    def test_the_default_is_four_not_unbounded(self):
        calls = self.run_recipe(None)
        self.assertIn("--build build/android-arm64 -j 4", calls)
        self.assertNotIn("--build build/android-arm64 -j", calls, "a bare -j means every core")


if __name__ == "__main__":
    unittest.main()
