"""launch-client.sh: where it finds the game, and that only one client runs at a time.

The script runs against a fake game install and fake `wine`/`pgrep`/`wineserver`/`Xephyr` on PATH,
so these tests never start the real game. NEVR_EVIDENCE_DELAY=0 skips the script's 12 s wait.
"""
from __future__ import annotations

import fcntl
import getpass
import os
import pathlib
import shutil
import subprocess
import tempfile
import unittest

REPO = pathlib.Path(__file__).resolve().parents[2]
SCRIPT = REPO / "launch-client.sh"
ORIGINAL = b"original-dll"
TEST_DLL = b"test-dll"


def make_game_root(root: pathlib.Path) -> pathlib.Path:
    """A fake main checkout: echovr/bin/win10 with the original DLL, an empty _local and a log dir."""
    win10 = root / "echovr/bin/win10"
    win10.mkdir(parents=True)
    (win10 / "BugSplat64.dll").write_bytes(ORIGINAL)
    (root / "echovr/_local").mkdir()
    (root / "echovr/.wineprefix/drive_c/users" / getpass.getuser() / "AppData/Local/EchoVR/logs").mkdir(
        parents=True)
    return root


def make_fake_bin(directory: pathlib.Path) -> None:
    """pgrep: Xephyr is up; echovr.exe is "running" iff FAKE_ECHOVR_PIDS is set AND the caller matches
    on the command line (-f), because the real game's process name is "Main Thread", so a `-x echovr.exe`
    match finds nothing. wine writes the log a logged-in run leaves. wineserver and the rest do nothing."""
    directory.mkdir()
    (directory / "pgrep").write_text(
        '#!/bin/bash\n'
        'for a in "$@"; do\n'
        '  if [[ "$a" == "Xephyr :101" ]]; then exit 0; fi\n'
        'done\n'
        'matches_cmdline=0\n'
        'for a in "$@"; do [[ "$a" == "-f" ]] && matches_cmdline=1; done\n'
        'if [[ -n "${FAKE_ECHOVR_PIDS:-}" && $matches_cmdline == 1 ]]; then echo "$FAKE_ECHOVR_PIDS"; exit 0; fi\n'
        'exit 1\n')
    (directory / "wine").write_text(
        '#!/bin/bash\n'
        'logs="$WINEPREFIX/drive_c/users/$(id -un)/AppData/Local/EchoVR/logs"\n'
        'echo \'{"msg":"NetGame switching state (from logging in, to logged in)"}\' > "$logs/nevr-fake.jsonl"\n'
        'cmp -s "$FAKE_EXPECT_DLL" "./BugSplat64.dll" || { echo "wrong DLL deployed" >&2; exit 9; }\n'
        '(sleep 3) &  # a leftover child, like a lingering wineserver, must not keep the lock\n'
        'exit 0\n')
    (directory / "wineserver").write_text('#!/bin/bash\nexit 0\n')
    for f in directory.iterdir():
        f.chmod(0o755)


class LaunchClientTest(unittest.TestCase):
    def setUp(self):
        self.tmp = pathlib.Path(tempfile.mkdtemp(prefix="launch-client-test-", dir="/var/tmp"))
        self.addCleanup(shutil.rmtree, self.tmp)
        self.game_root = make_game_root(self.tmp / "main")
        self.fake_bin = self.tmp / "bin"
        make_fake_bin(self.fake_bin)
        self.dll = self.tmp / "test.dll"
        self.dll.write_bytes(TEST_DLL)
        # A checkout with the script but NO echovr/ of its own, like a worktree.
        self.checkout = self.tmp / "checkout"
        self.checkout.mkdir()
        shutil.copy(SCRIPT, self.checkout / "launch-client.sh")
        self.lock = self.tmp / "launch.lock"
        self.env = dict(
            os.environ,
            PATH=f"{self.fake_bin}:{os.environ['PATH']}",
            NEVR_GAME_ROOT=str(self.game_root),
            NEVR_LAUNCH_LOCK=str(self.lock),
            NEVR_EVIDENCE_DELAY="0",
            NEVR_RUN_SCRATCH_ROOT=str(self.tmp),
            FAKE_EXPECT_DLL=str(self.dll),
        )
        self.env.pop("FAKE_ECHOVR_PIDS", None)

    def run_script(self, *args, env=None, cwd=None):
        return subprocess.run([str(self.checkout / "launch-client.sh"), *args], env=env or self.env,
                              cwd=cwd or self.checkout, capture_output=True, text=True, timeout=60)

    def deployed(self) -> bytes:
        return (self.game_root / "echovr/bin/win10/BugSplat64.dll").read_bytes()

    def test_runs_from_a_checkout_without_echovr_and_restores_the_original(self):
        result = self.run_script("--dll", str(self.dll))
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("PASS: client reached logged in", result.stdout)
        self.assertIn("original BugSplat64.dll restored and verified", result.stdout)
        self.assertEqual(self.deployed(), ORIGINAL)

    def test_refuses_to_start_while_echovr_is_running(self):
        env = dict(self.env, FAKE_ECHOVR_PIDS="4242")
        result = self.run_script("--dll", str(self.dll), env=env)
        self.assertEqual(result.returncode, 4, result.stdout + result.stderr)
        self.assertIn("echovr.exe is already running (pid 4242)", result.stderr)
        self.assertEqual(self.deployed(), ORIGINAL)  # nothing was deployed
        self.assertNotIn("Deploying", result.stdout)

    def test_refuses_to_start_while_another_run_holds_the_lock(self):
        with open(self.lock, "w") as held:
            fcntl.flock(held, fcntl.LOCK_EX | fcntl.LOCK_NB)
            result = self.run_script("--dll", str(self.dll))
        self.assertEqual(result.returncode, 4, result.stdout + result.stderr)
        self.assertIn("another launch-client.sh run holds", result.stderr)
        self.assertEqual(self.deployed(), ORIGINAL)

    def test_the_lock_is_released_when_the_run_ends_even_if_a_child_outlives_it(self):
        self.assertEqual(self.run_script("--dll", str(self.dll)).returncode, 0)
        self.assertEqual(self.run_script("--dll", str(self.dll)).returncode, 0)

    def test_a_missing_game_install_is_a_clear_error(self):
        env = dict(self.env, NEVR_GAME_ROOT=str(self.tmp / "nowhere"))
        result = self.run_script("--dll", str(self.dll), env=env)
        self.assertEqual(result.returncode, 2)
        self.assertIn("no game install at", result.stderr)

    def test_a_relative_game_root_is_rejected(self):
        env = dict(self.env, NEVR_GAME_ROOT="relative/dir")
        result = self.run_script("--dll", str(self.dll), env=env)
        self.assertEqual(result.returncode, 2)
        self.assertIn("must be an absolute path", result.stderr)

    def test_an_unwritable_lock_is_a_clear_error(self):
        env = dict(self.env, NEVR_LAUNCH_LOCK=str(self.tmp / "no-such-dir-file" / "x" / "lock"))
        (self.tmp / "no-such-dir-file").write_text("a file, so mkdir -p of its child fails")
        result = self.run_script("--dll", str(self.dll), env=env)
        self.assertNotEqual(result.returncode, 0)

    def test_print_game_root_honours_the_override(self):
        result = self.run_script("--print-game-root")
        self.assertEqual(result.stdout.strip(), str(self.game_root))

    def test_print_game_root_from_a_worktree_is_the_main_checkout(self):
        main = self.tmp / "repo"
        main.mkdir()
        git = ["git", "-c", "user.name=t", "-c", "user.email=t@example.invalid", "-C", str(main)]
        subprocess.run([*git, "init", "-q"], check=True)
        subprocess.run([*git, "commit", "-q", "--allow-empty", "-m", "init", "--no-gpg-sign"], check=True)
        worktree = self.tmp / "wt"
        subprocess.run([*git, "worktree", "add", "-q", str(worktree)], check=True)
        env = {k: v for k, v in self.env.items() if k != "NEVR_GAME_ROOT"}
        for where in (main, worktree):
            shutil.copy(SCRIPT, where / "launch-client.sh")
            out = subprocess.run([str(where / "launch-client.sh"), "--print-game-root"], env=env,
                                 cwd=where, capture_output=True, text=True, check=True)
            self.assertEqual(out.stdout.strip(), str(main.resolve()), f"from {where}")

    def test_the_scenario_runner_asks_the_script_for_the_prefix(self):
        import sys
        sys.path.insert(0, str(REPO / "tools/scenario"))
        import run_scenario
        old = os.environ.get("NEVR_GAME_ROOT")
        os.environ["NEVR_GAME_ROOT"] = str(self.game_root)
        try:
            self.assertEqual(run_scenario.wineprefix(), self.game_root / "echovr/.wineprefix")
        finally:
            if old is None:
                del os.environ["NEVR_GAME_ROOT"]
            else:
                os.environ["NEVR_GAME_ROOT"] = old


if __name__ == "__main__":
    unittest.main()
