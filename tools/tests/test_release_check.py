"""tools/release_check.py: every release input is checked up front, by name, and no value is printed (#1997)."""
from __future__ import annotations

import os
import pathlib
import stat
import sys
import tempfile
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent.parent))
import release_check  # noqa: E402


class ReleaseCheckTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        base = pathlib.Path(self.tmp.name)
        bindir = base / "bin"
        bindir.mkdir()
        for program in release_check.PROGRAMS:
            p = bindir / program
            p.write_text("#!/bin/sh\n")
            p.chmod(p.stat().st_mode | stat.S_IXUSR)
        self.env = {"PATH": str(bindir)}
        for name, is_file, _ in release_check.INPUTS:
            if is_file:
                f = base / f"{name}.file"
                f.write_text("x")
                self.env[name] = str(f)
            else:
                self.env[name] = f"value-of-{name}"
        self.root = base

    def tearDown(self):
        self.tmp.cleanup()

    def check(self, env):
        return release_check.problems(env, self.root, run_preflight=False)

    def test_everything_present_is_ready(self):
        self.assertEqual(self.check(self.env), [])

    def test_each_missing_input_is_named(self):
        for name, _, _ in release_check.INPUTS:
            env = dict(self.env)
            del env[name]
            found = self.check(env)
            self.assertEqual(len(found), 1, name)
            self.assertIn(name, found[0])
            self.assertIn("not set", found[0])

    def test_an_empty_input_is_missing(self):
        env = dict(self.env, R2_BUCKET="")
        self.assertEqual(self.check(env), ["R2_BUCKET is not set (the release host's bucket); add it to .env"])

    def test_a_file_input_that_names_no_file_is_named(self):
        env = dict(self.env, NEVR_QUEST_REFERENCE_APK=str(self.root / "absent.apk"))
        found = self.check(env)
        self.assertEqual(len(found), 1)
        self.assertIn("NEVR_QUEST_REFERENCE_APK names no file", found[0])

    def test_a_missing_program_is_named(self):
        (self.root / "bin" / "zipalign").unlink()
        self.assertEqual(self.check(self.env), ["zipalign is not on PATH"])

    def test_every_problem_is_reported_not_just_the_first(self):
        env = dict(self.env)
        del env["R2_BUCKET"]
        del env["GH_TOKEN"]
        (self.root / "bin" / "gh").unlink()
        self.assertEqual(len(self.check(env)), 3)

    def test_no_value_is_ever_printed(self):
        env = {k: v for k, v in self.env.items() if k == "PATH" or not k.startswith("R2_")}
        env["NEVR_QUEST_REFERENCE_APK"] = "/no/such/secret-path.apk"
        found = "\n".join(self.check(env))
        for name, value in self.env.items():
            if name != "PATH" and not os.path.isabs(value):
                self.assertNotIn(value, found)
        self.assertNotIn("secret-path", found)

    def test_env_example_lists_every_input(self):
        example = (pathlib.Path(__file__).resolve().parent.parent.parent / ".env.example").read_text()
        for name, _, _ in release_check.INPUTS:
            self.assertIn(f"\n{name}=", example, name)


if __name__ == "__main__":
    unittest.main()
