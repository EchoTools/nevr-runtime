"""A throwaway game copy carries no sign-in (#1198): launch-client.sh, launch-server.sh and verify-server.sh
refuse to deploy or start the game when NEVR_GAME_ROOT names a root that holds any `.credentials.json*` file,
whether in `_local` or in the nevr root of the Wine prefix.

Nothing here starts the game: the scripts run for real against the fake install and fake `wine` of
test_launch_client.py. The credential files are fakes made in a temporary directory; their content is a marker
that must never reach the output.
"""
from __future__ import annotations

import getpass
import os
import pathlib
import re
import shutil
import subprocess
import sys
import tempfile
import unittest

REPO = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "tools"))

import test_launch_client as fixtures  # noqa: E402

MARKER = "SECRET-MARKER-MUST-NOT-BE-PRINTED"
NEVR_ROOT_RELATIVE = pathlib.Path("echovr/.wineprefix/drive_c/users") / getpass.getuser() / "AppData/Local/nevr/echovr"
# (where, relative to the game root): the two kinds of sign-in file a copy carried.
LOCATIONS = {
    "local": pathlib.Path("echovr/_local/.credentials.json"),
    "local-backup": pathlib.Path("echovr/_local/.credentials.json.prod-bak"),
    "nevr-root": NEVR_ROOT_RELATIVE / ".credentials.json",
}
SCRIPTS = ("launch-client.sh", "launch-server.sh", "verify-server.sh")


class CopyCredentialsGuardTest(unittest.TestCase):
    def setUp(self):
        self.tmp = pathlib.Path(tempfile.mkdtemp(prefix="copy-credentials-test-", dir="/var/tmp"))
        self.addCleanup(shutil.rmtree, self.tmp)
        self.game_root = fixtures.make_game_root(self.tmp / "copy")
        self.fake_bin = self.tmp / "bin"
        fixtures.make_fake_bin(self.fake_bin)
        self.checkout = self.tmp / "checkout"
        self.checkout.mkdir()
        fixtures.install_scripts(self.checkout)
        bin_dir = self.checkout / "build/mingw-release/bin"
        bin_dir.mkdir(parents=True)
        (bin_dir / "BugSplat64.dll").write_bytes(fixtures.TEST_DLL)
        (bin_dir / "nevr-runtime.dll").write_bytes(fixtures.TEST_RUNTIME)
        self.win10 = self.game_root / "echovr/bin/win10"
        self.lock = self.tmp / "launch.lock"
        self.started = self.tmp / "wine-started"
        self.env = dict(
            os.environ,
            PATH=f"{self.fake_bin}:{os.environ['PATH']}",
            NEVR_GAME_ROOT=str(self.game_root),
            NEVR_LAUNCH_LOCK=str(self.lock),
            NEVR_RUN_SCRATCH_ROOT=str(self.tmp),
            NEVR_WORK_ROOT=str(self.tmp / "work"),
            NEVR_EVIDENCE_DELAY="0",
            NEVR_VERIFY_MIN_SECONDS="1",
            NEVR_VERIFY_POLL_SECONDS="1",
            FAKE_EXPECT_DLL=str(bin_dir / "BugSplat64.dll"),
            FAKE_EXPECT_RUNTIME=str(bin_dir / "nevr-runtime.dll"),
            FAKE_WINE_STARTED=str(self.started),
        )
        self.env.pop("FAKE_ECHOVR_PIDS", None)

    def command(self, script: str) -> list[str]:
        path = str(self.checkout / script)
        if script == "launch-client.sh":
            # --device-code: the run has no stored sign-in to name, so the guard under test is the one that refuses.
            return [path, "--dll", str(self.checkout / "build/mingw-release/bin/BugSplat64.dll"), "--device-code"]
        if script == "verify-server.sh":
            return [path, "run1", "default", "1"]
        return [path]

    def run_script(self, script: str, env=None) -> subprocess.CompletedProcess:
        return subprocess.run(self.command(script), env=env or self.env, cwd=self.checkout, capture_output=True,
                              text=True, timeout=60)

    def plant(self, where: str, root: pathlib.Path | None = None) -> pathlib.Path:
        path = (root or self.game_root) / LOCATIONS[where]
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(MARKER)
        return path

    def assert_nothing_deployed_or_started(self):
        self.assertEqual((self.win10 / "BugSplat64.dll").read_bytes(), fixtures.ORIGINAL, "the host DLL was replaced")
        self.assertFalse((self.win10 / "nevr-runtime.dll").exists(), "the runtime was deployed")
        self.assertFalse((self.win10 / "plugins").exists(), "a plugin directory was made")
        self.assertFalse(self.started.exists(), "the game was started")
        self.assertFalse(self.lock.exists(), "the run lock was taken before the refusal")
        self.assertFalse((self.tmp / "README.md").exists(), "the lock's work directory was made before the refusal")
        self.assertFalse((self.tmp / "work").exists(), "the work root was written before the refusal")
        self.assertFalse((self.tmp / "server-runs").exists(), "a run's output directory was made")
        self.assertEqual(list(self.tmp.glob("client-run-*")), [], "a run's scratch directory was made")

    def test_each_script_refuses_each_kind_of_sign_in_file_and_names_its_path(self):
        for script in SCRIPTS:
            for where in LOCATIONS:
                with self.subTest(script=script, where=where):
                    path = self.plant(where)
                    result = self.run_script(script)
                    path.unlink()
                    self.assertEqual(result.returncode, 3, result.stdout + result.stderr)
                    self.assertIn(str(path), result.stderr)
                    self.assertEqual(len([ln for ln in result.stderr.splitlines() if str(path) in ln]), 1)
                    self.assertNotIn(MARKER, result.stdout + result.stderr, "the file's content was printed")
                    self.assert_nothing_deployed_or_started()

    def test_every_file_is_named_on_the_one_line(self):
        local = self.plant("local")
        backup = self.plant("local-backup")
        nevr = self.plant("nevr-root")
        result = self.run_script("launch-server.sh")
        self.assertEqual(result.returncode, 3, result.stdout + result.stderr)
        lines = [ln for ln in result.stderr.splitlines() if "ABORT" in ln]
        self.assertEqual(len(lines), 1, result.stderr)
        for path in (local, backup, nevr):
            self.assertIn(str(path), lines[0])
        self.assert_nothing_deployed_or_started()

    def test_a_dangling_link_named_like_a_sign_in_file_is_refused(self):
        link = self.game_root / LOCATIONS["local"]
        link.symlink_to(self.tmp / "nowhere")
        result = self.run_script("launch-server.sh")
        self.assertEqual(result.returncode, 3, result.stdout + result.stderr)
        self.assertIn(str(link), result.stderr)
        self.assert_nothing_deployed_or_started()

    def test_a_copy_without_sign_in_files_runs_every_script(self):
        for script in SCRIPTS:
            with self.subTest(script=script):
                # launch-server.sh leaves its deployed DLLs behind, so every script gets a fresh copy.
                shutil.rmtree(self.game_root)
                fixtures.make_game_root(self.game_root)
                result = self.run_script(script)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                self.assertTrue(self.started.exists(), "the game was not started")
                self.started.unlink()

    def test_other_files_in_the_copy_are_not_sign_in_files(self):
        (self.game_root / "echovr/_local/credentials.txt").write_text(MARKER)
        (self.game_root / NEVR_ROOT_RELATIVE / "credentials.json").write_text(MARKER)
        (self.game_root / "echovr/_local/.credentials.json.d").mkdir()
        result = self.run_script("launch-server.sh")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_the_real_install_is_not_checked_when_no_copy_is_named(self):
        # NEVR_GAME_ROOT unset: the game root is the main checkout, whose echovr/ is the real install, which
        # legitimately holds a sign-in. A fake checkout that is not a git repository resolves to itself.
        fixtures.make_game_root(self.checkout)
        path = self.plant("nevr-root", self.checkout)
        env = {k: v for k, v in self.env.items() if k != "NEVR_GAME_ROOT"}
        result = self.run_script("launch-server.sh", env=env)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertTrue(path.exists(), "the real install's sign-in was touched")
        self.assertTrue(self.started.exists())

    def test_a_search_that_fails_is_not_read_as_no_sign_in_file(self):
        # A find that cannot finish (an unreadable directory) prints nothing; "nothing found" must not be inferred.
        (self.fake_bin / "find").write_text("#!/bin/bash\necho 'find: fake failure' >&2\nexit 1\n")
        (self.fake_bin / "find").chmod(0o755)
        result = self.run_script("launch-server.sh")
        self.assertEqual(result.returncode, 2, result.stdout + result.stderr)
        self.assertIn("cannot search", result.stderr)
        self.assert_nothing_deployed_or_started()

    def test_the_lock_without_a_game_root_is_an_error_and_takes_no_lock(self):
        result = subprocess.run(["bash", "-c", f"set -eu; source {fixtures.LIB}; acquire_game_run_lock"],
                                env=self.env, capture_output=True, text=True, timeout=30)
        self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("acquire_game_run_lock needs GAME_ROOT", result.stderr)
        self.assertFalse(self.lock.exists())

    def test_the_find_lines_of_the_recipe_in_agents_md_list_then_remove_every_sign_in_file(self):
        # The "Make it safe" step of AGENTS.md, run as written on a fake copy: the files are listed, removed, and the
        # last listing is empty. The lines are read from the document, so a command that stops working fails here.
        text = (REPO / "AGENTS.md").read_text()
        lines = [ln.strip() for ln in text.splitlines()
                 if ln.strip().startswith("find <copy>/echovr -name '.credentials.json*'")]
        self.assertEqual(len(lines), 3, lines)
        planted = [self.plant(where) for where in LOCATIONS]
        (self.game_root / "echovr/_local/keep.json").write_text("{}")
        outputs = []
        for line in lines:
            done = subprocess.run(["bash", "-c", line.replace("<copy>", str(self.game_root))], capture_output=True,
                                  text=True, timeout=30)
            self.assertEqual(done.returncode, 0, done.stderr)
            outputs.append(sorted(done.stdout.splitlines()))
        self.assertEqual(outputs[0], sorted(str(path) for path in planted))
        self.assertEqual(outputs[1], [])
        self.assertEqual(outputs[2], [])
        self.assertTrue(all(not path.exists() for path in planted))
        self.assertTrue((self.game_root / "echovr/_local/keep.json").exists(), "an unrelated file was removed")

    def test_every_script_that_finds_the_install_goes_through_the_one_guard(self):
        # A launcher added later that sources the shared helper must take the run lock, which carries the guard.
        sources = [p for p in REPO.glob("*.sh") if "tools/lib/game_install.sh" in p.read_text()]
        self.assertEqual(sorted(p.name for p in sources), sorted(SCRIPTS))
        for path in sources:
            with self.subTest(script=path.name):
                text = re.sub(r"(?m)^\s*#.*$", "", path.read_text())
                self.assertRegex(text, r"(?m)^acquire_game_run_lock$")
        helper = re.sub(r"(?m)^\s*#.*$", "", (REPO / "tools/lib/game_install.sh").read_text())
        body = helper[helper.index("acquire_game_run_lock() {"):]
        self.assertLess(body.index("refuse_copy_credentials"), body.index("work_dir"),
                        "the guard must run before the lock's directory is made and its file opened")


if __name__ == "__main__":
    unittest.main()
