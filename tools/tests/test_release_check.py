"""tools/release_check.py: every release input is checked up front, by name, and no value is printed (#1997)."""
from __future__ import annotations

import os
import pathlib
import shutil
import stat
import subprocess
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
                f.write_text("#!/bin/sh\n")
                f.chmod(0o700)
                self.env[name] = str(f)
            else:
                self.env[name] = f"value-of-{name}"
        self.root = base
        self.pins = {name: release_check._sha256(pathlib.Path(self.env[name])) for name in release_check.PINS}

    def tearDown(self):
        self.tmp.cleanup()

    def check(self, env):
        return release_check.problems(env, self.root, run_preflight=False, pins=self.pins)

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
        env = dict(self.env)
        for name, is_file, _ in release_check.INPUTS:
            env[name] = f"/no/such/{name}-secret-value" if is_file else f"{name}-secret-value"
        (self.root / "bin" / "zip").unlink()
        found = "\n".join(self.check(env))
        self.assertTrue(found)
        self.assertNotIn("secret-value", found)

    def test_an_unreadable_file_is_named(self):
        path = pathlib.Path(self.env["NEVR_QUEST_KEYSTORE"])
        path.chmod(0)
        try:
            if os.access(path, os.R_OK):
                self.skipTest("running as a user that reads mode-0 files")
            found = self.check(self.env)
        finally:
            path.chmod(0o600)
        self.assertEqual(found, ["NEVR_QUEST_KEYSTORE names a file this user can't read (the Quest APK signing keystore)"])

    def test_a_tilde_path_is_taken_as_written(self):
        home = self.root / "home"
        home.mkdir()
        (home / "keystore.jks").write_text("x")
        env = dict(self.env, NEVR_QUEST_KEYSTORE="~/keystore.jks")
        old = os.environ.get("HOME")
        os.environ["HOME"] = str(home)
        try:
            found = self.check(env)
        finally:
            if old is None:
                del os.environ["HOME"]
            else:
                os.environ["HOME"] = old
        self.assertEqual(len(found), 1)
        self.assertIn("NEVR_QUEST_KEYSTORE names no file", found[0])

    def test_a_signer_that_is_not_executable_is_named(self):
        pathlib.Path(self.env["RELEASE_SIGNER_BIN"]).chmod(0o644)
        found = self.check(self.env)
        self.assertEqual(len(found), 1)
        self.assertIn("RELEASE_SIGNER_BIN names no executable file", found[0])

    def test_a_signer_named_on_path_is_ready(self):
        env = dict(self.env, RELEASE_SIGNER_BIN="git")
        self.assertEqual(self.check(env), [])

    def test_a_credential_in_the_origin_url_is_redacted(self):
        tools = pathlib.Path(__file__).resolve().parent.parent
        root = self.root / "clone"
        (root / "tools").mkdir(parents=True)
        for name in ("release_preflight.py", "quest_apk_version.py"):
            shutil.copy(tools / name, root / "tools" / name)
        subprocess.run(["git", "init", "-q", str(root)], check=True)
        subprocess.run(["git", "-C", str(root), "remote", "add", "origin",
                        "https://someone:ghp_FAKEcredential@example.invalid/x/y.git"], check=True)
        env = dict(os.environ, XDG_STATE_HOME=str(self.root / "state"), GIT_TERMINAL_PROMPT="0")
        found = release_check.problems(env, root, run_preflight=True)
        preflight = [p for p in found if p.startswith("preflight:")]
        self.assertTrue(preflight, found)
        self.assertNotIn("ghp_FAKEcredential", "\n".join(found))
        self.assertIn("<redacted>@", "\n".join(preflight))

    def test_redaction_covers_an_at_sign_in_the_password(self):
        from release_preflight import redact_url
        self.assertEqual(redact_url("https://u:p@ss@example.invalid/x"), "https://<redacted>@example.invalid/x")
        self.assertEqual(redact_url("git@github.com:EchoTools/nevr-runtime.git"),
                         "git@github.com:EchoTools/nevr-runtime.git")
        self.assertEqual(redact_url("https://example.invalid/a@b"), "https://example.invalid/a@b")

    def test_a_file_that_is_not_its_pin_is_named(self):
        pathlib.Path(self.env["NEVR_QUEST_REFERENCE_APK"]).write_text("the store APK, say")
        found = self.check(self.env)
        self.assertEqual(len(found), 1)
        self.assertIn("NEVR_QUEST_REFERENCE_APK is not the pinned file", found[0])

    def test_the_default_pins_are_applied(self):
        pathlib.Path(self.env["NEVR_QUEST_REFERENCE_APK"]).write_text("not the reference build")
        found = release_check.problems(self.env, self.root, run_preflight=False)
        self.assertTrue(any("NEVR_QUEST_REFERENCE_APK is not the pinned file" in p for p in found), found)

    def test_the_reference_pin_is_the_builds_own(self):
        import quest_release_base
        self.assertEqual(release_check.PINS["NEVR_QUEST_REFERENCE_APK"], quest_release_base.REFERENCE_APK_SHA256)

    def test_env_example_lists_every_input(self):
        example = (pathlib.Path(__file__).resolve().parent.parent.parent / ".env.example").read_text()
        for name, _, _ in release_check.INPUTS:
            self.assertIn(f"\n{name}=", example, name)


if __name__ == "__main__":
    unittest.main()
