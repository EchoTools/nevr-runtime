"""launch-client.sh and verify-server.sh: where they find the game, that only one game run happens
at a time, and that verify-server.sh puts back what it deployed.

The script runs against a fake game install and fake `wine`/`pgrep`/`wineserver`/`Xephyr` on PATH,
so these tests never start the real game. NEVR_EVIDENCE_DELAY=0 skips the script's 12 s wait.
"""
from __future__ import annotations

import fcntl
import getpass
import os
import pathlib
import shutil
import signal
import subprocess
import time
import tempfile
import unittest

REPO = pathlib.Path(__file__).resolve().parents[2]
SCRIPT = REPO / "launch-client.sh"
VERIFY_SERVER = REPO / "verify-server.sh"
LAUNCH_SERVER = REPO / "launch-server.sh"
LIB = REPO / "tools/lib/game_install.sh"
ORIGINAL = b"original-dll"
TEST_DLL = b"test-dll"


def install_scripts(checkout: pathlib.Path) -> None:
    """Copy the scripts under test and the helper they source into a fake checkout."""
    (checkout / "tools/lib").mkdir(parents=True, exist_ok=True)
    shutil.copy(LIB, checkout / "tools/lib/game_install.sh")
    for script in (SCRIPT, VERIFY_SERVER, LAUNCH_SERVER):
        shutil.copy(script, checkout / script.name)


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
        'sleep "${FAKE_WINE_LOG_DELAY:-0}"\n'
        'if [[ -n "${FAKE_LOG_TEXT:-}" ]]; then printf "%b" "$FAKE_LOG_TEXT" > "$logs/nevr-fake-$(date +%s%N).jsonl"\n'
        'else echo \'{"msg":"NetGame switching state (from logging in, to logged in)"}\' > "$logs/nevr-fake-$(date +%s%N).jsonl"; fi\n'
        'cmp -s "$FAKE_EXPECT_DLL" "./BugSplat64.dll" || { echo "wrong DLL deployed" >&2; exit 9; }\n'
        '(sleep 3) &  # a leftover child, like a lingering wineserver, must not keep the lock\n'
        '# A directory where the DLL was makes the restore fail for any user; chmod 444 does not stop root,\n'
        '# which is who runs the tests in the CI container.\n'
        '[[ -n "${FAKE_WINE_LOCK_DLL:-}" ]] && { rm -f ./BugSplat64.dll; mkdir ./BugSplat64.dll; }\n'
        'exec sleep "${FAKE_WINE_SLEEP:-0}"\n')
    # wineserver: like the real one, `-k` exits 1 when no server is left (every call after the first), `-w`
    # exits 0; each call is logged with the deployed DLL's content so tests can see WHEN it ran.
    (directory / "wineserver").write_text(
        '#!/bin/bash\n'
        '[[ -n "${FAKE_WS_LOG:-}" ]] && echo "$* dll=$(cat "$FAKE_WS_DLL" 2>/dev/null)" >> "$FAKE_WS_LOG"\n'
        'if [[ "$1" == "-k" && -n "${FAKE_WS_STATE:-}" ]]; then\n'
        '  if [[ -e "$FAKE_WS_STATE" ]]; then exit 1; fi\n'
        '  touch "$FAKE_WS_STATE"\n'
        'fi\n'
        'exit 0\n')
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
        install_scripts(self.checkout)
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

    def run_until_login(self, log_text=None, *extra, timeout_flag=("--login-timeout", "45"), env_extra=None):
        env = dict(self.env, FAKE_WINE_SLEEP="6", NEVR_LOGIN_POLL_SECONDS="1")
        if log_text is not None:
            env["FAKE_LOG_TEXT"] = log_text
        env.update(env_extra or {})
        started = time.monotonic()
        result = self.run_script("--dll", str(self.dll), "--exit-after-login", *timeout_flag, *extra, env=env)
        return result, time.monotonic() - started

    def test_exit_after_login_ends_the_run_itself_and_restores(self):
        result, elapsed = self.run_until_login()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("PASS: client reached logged in", result.stdout)
        self.assertLess(elapsed, 5.5, "it waited for the game to exit instead of ending the run at the verdict")
        self.assertEqual(self.deployed(), ORIGINAL)

    def test_exit_after_login_fails_when_the_service_stays_unavailable(self):
        result, elapsed = self.run_until_login('{"msg":"Service is unavailable"}\\n' * 3)
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn("client never reached logged in", result.stderr)
        self.assertLess(elapsed, 5.5)
        self.assertEqual(self.deployed(), ORIGINAL)

    def test_exit_after_login_fails_at_the_deadline_when_nothing_happens(self):
        result, elapsed = self.run_until_login('{"msg":"noise"}\\n', timeout_flag=("--login-timeout", "2"),
                                               env_extra={"NEVR_LOGIN_MIN_SECONDS": "1", "FAKE_WINE_SLEEP": "30"})
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn("client never reached logged in", result.stderr)
        self.assertGreaterEqual(elapsed, 2.0)
        self.assertLess(elapsed, 12.0)
        self.assertEqual(self.deployed(), ORIGINAL)

    def test_the_deadline_is_not_short_when_the_run_starts_late_in_a_second(self):
        # Whole-second arithmetic once ended a 2 s deadline after 1.04 s when the script read its
        # start at x.99. A `date +%s` whose clock reads x.99 the first time it is asked puts every run
        # in that position, so the outcome does not depend on when the test happens to start.
        clock = self.tmp / "clock-shift"
        fake_date = self.fake_bin / "date"
        fake_date.write_text(
            '#!/bin/bash\n'
            'if [[ "$1" != "+%s" ]]; then exec /usr/bin/date "$@"; fi\n'
            'now_us=${EPOCHREALTIME/./}\n'
            f'[[ -e {clock} ]] || echo $((990000 - now_us % 1000000)) > {clock}\n'
            f'echo $(( (now_us + $(cat {clock})) / 1000000 ))\n')
        fake_date.chmod(0o755)
        result, elapsed = self.run_until_login('{"msg":"noise"}\\n', timeout_flag=("--login-timeout", "2"),
                                               env_extra={"NEVR_LOGIN_MIN_SECONDS": "1", "FAKE_WINE_SLEEP": "30"})
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertGreaterEqual(elapsed, 2.0)

    def test_a_dll_that_embeds_no_endpoints_is_refused_even_if_login_appears(self):
        text = ('{"msg":"[NEVR.CONFIG] built-in defaults embedded in this build: (none)"}\\n'
                '{"msg":"NetGame switching state (from logging in, to logged in)"}\\n')
        result, _ = self.run_until_login(text)
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn("embeds no service endpoints", result.stderr)

    def test_a_service_that_keeps_failing_ends_the_run_early(self):
        result, elapsed = self.run_until_login('{"msg":"rad15_live failed"}\\n' * 3)
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn("client never reached logged in", result.stderr)
        self.assertLess(elapsed, 5.5)

    def test_a_dll_that_embeds_nothing_ends_the_run_without_waiting_for_the_deadline(self):
        result, elapsed = self.run_until_login('{"msg":"[NEVR.CONFIG] built-in defaults embedded in this build: (none)"}\\n')
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn("embeds no service endpoints", result.stderr)
        self.assertLess(elapsed, 5.5)

    def test_a_previous_runs_log_from_the_same_second_cannot_decide_the_verdict(self):
        logs = self.game_root / "echovr/.wineprefix/drive_c/users" / getpass.getuser() / "AppData/Local/EchoVR/logs"
        stale = logs / "nevr-2026-10-07T00-00-00.000.jsonl"
        stale.write_text('{"msg":"NetGame switching state (from logging in, to logged in)"}\n')
        # This run's own log has no login at all; the stale file must not be mistaken for it.
        result, _ = self.run_until_login('{"msg":"noise"}\\n', timeout_flag=("--login-timeout", "2"),
                                         env_extra={"NEVR_LOGIN_MIN_SECONDS": "1", "FAKE_WINE_SLEEP": "30",
                                                    "FAKE_WINE_LOG_DELAY": "1"})
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn("client never reached logged in", result.stderr)
        self.assertNotIn("nevr-2026-10-07T00-00-00.000.jsonl", result.stdout)

    def test_the_wine_server_is_stopped_and_awaited_before_the_dll_is_restored(self):
        ws_log = self.tmp / "ws.log"
        result, _ = self.run_until_login(env_extra={
            "FAKE_WS_LOG": str(ws_log), "FAKE_WS_STATE": str(self.tmp / "ws.state"),
            "FAKE_WS_DLL": str(self.game_root / "echovr/bin/win10/BugSplat64.dll")})
        # The real wineserver exits 1 when nothing is left to kill: that must not become the exit status.
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        calls = [l for l in ws_log.read_text().splitlines() if l]
        self.assertEqual(calls[:2], ["-k dll=test-dll", "-w dll=test-dll"], calls)  # both before the restore
        self.assertEqual(self.deployed(), ORIGINAL)

    def test_a_login_timeout_without_the_flag_is_not_validated(self):
        result = self.run_script("--dll", str(self.dll), "--login-timeout", "10")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_a_login_timeout_below_the_minimum_patience_is_rejected(self):
        result = self.run_script("--dll", str(self.dll), "--exit-after-login", "--login-timeout", "10")
        self.assertEqual(result.returncode, 2, result.stdout + result.stderr)
        self.assertIn("--login-timeout must be", result.stderr)
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
        self.assertIn("another game run (launch-client.sh, launch-server.sh or verify-server.sh) holds", result.stderr)
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
            install_scripts(where)
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


class VerifyServerTest(unittest.TestCase):
    """verify-server.sh deploys the test DLL and a plugin, runs a (fake) game, and restores."""

    def setUp(self):
        self.tmp = pathlib.Path(tempfile.mkdtemp(prefix="verify-server-test-", dir="/var/tmp"))
        self.addCleanup(shutil.rmtree, self.tmp)
        self.game_root = make_game_root(self.tmp / "main")
        self.fake_bin = self.tmp / "bin"
        make_fake_bin(self.fake_bin)
        self.checkout = self.tmp / "checkout"
        self.checkout.mkdir()
        install_scripts(self.checkout)
        bin_dir = self.checkout / "build/mingw-release/bin"
        (bin_dir / "plugins").mkdir(parents=True)
        (bin_dir / "BugSplat64.dll").write_bytes(TEST_DLL)
        (bin_dir / "plugins/extra.dll").write_bytes(b"plugin")
        self.win10 = self.game_root / "echovr/bin/win10"
        self.env = dict(
            os.environ,
            PATH=f"{self.fake_bin}:{os.environ['PATH']}",
            NEVR_GAME_ROOT=str(self.game_root),
            NEVR_LAUNCH_LOCK=str(self.tmp / "launch.lock"),
            NEVR_RUN_SCRATCH_ROOT=str(self.tmp),
            NEVR_VERIFY_MIN_SECONDS="1",
            NEVR_VERIFY_POLL_SECONDS="1",
            FAKE_EXPECT_DLL=str(bin_dir / "BugSplat64.dll"),
        )
        self.env.pop("FAKE_ECHOVR_PIDS", None)

    def command(self):
        return [str(self.checkout / "verify-server.sh"), "run1", "default", "1"]

    def test_a_finished_run_leaves_the_game_directory_as_it_found_it(self):
        result = subprocess.run(self.command(), env=self.env, cwd=self.checkout, capture_output=True,
                                text=True, timeout=60)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual((self.win10 / "BugSplat64.dll").read_bytes(), ORIGINAL)
        self.assertFalse((self.win10 / "plugins/extra.dll").exists(), "a deployed plugin was left behind")
        self.assertIn("deployed files restored and verified", (self.tmp / "server-runs/run1/server.log").read_text())

    def test_an_interrupted_run_still_restores(self):
        env = dict(self.env, FAKE_WINE_SLEEP="30")
        process = subprocess.Popen(self.command(), env=env, cwd=self.checkout, stdout=subprocess.PIPE,
                                   stderr=subprocess.STDOUT, text=True)
        try:
            deadline = time.monotonic() + 30
            while (self.win10 / "BugSplat64.dll").read_bytes() != TEST_DLL:
                self.assertLess(time.monotonic(), deadline, "the test DLL was never deployed")
                time.sleep(0.05)
            process.send_signal(signal.SIGTERM)
            process.wait(timeout=30)
        finally:
            if process.poll() is None:
                process.kill()
            process.stdout.close()
        self.assertEqual((self.win10 / "BugSplat64.dll").read_bytes(), ORIGINAL)
        self.assertFalse((self.win10 / "plugins/extra.dll").exists())

    def run_verify(self, env=None):
        return subprocess.run(self.command(), env=env or self.env, cwd=self.checkout, capture_output=True,
                              text=True, timeout=60)

    def test_back_to_back_runs_both_succeed(self):
        # A child that outlives the game (the fake wine leaves a sleeping one) must not keep the lock.
        self.assertEqual(self.run_verify().returncode, 0)
        self.assertEqual(self.run_verify().returncode, 0)

    def wait_for_lock_release(self, timeout=30.0) -> float:
        """Seconds until nothing holds the run lock. SIGKILL stops the shell at once, but a child it had
        just started (a cp copying the DLL) can outlive it by a moment and, having inherited the lock
        descriptor, keeps the lock until it ends; a rerun in that moment is refused, correctly (#295)."""
        started = time.monotonic()
        while True:
            with open(self.tmp / "launch.lock", "a") as probe:
                try:
                    fcntl.flock(probe, fcntl.LOCK_EX | fcntl.LOCK_NB)
                    return time.monotonic() - started
                except OSError:
                    pass
            self.assertLess(time.monotonic() - started, timeout, "the killed run's lock was never released")
            time.sleep(0.02)

    def kill_after_deploy(self, env, also_wait_for=lambda: True):
        process = subprocess.Popen(self.command(), env=env, cwd=self.checkout, stdout=subprocess.DEVNULL,
                                   stderr=subprocess.DEVNULL)
        try:
            deadline = time.monotonic() + 30
            while (self.win10 / "BugSplat64.dll").read_bytes() != TEST_DLL or not also_wait_for():
                self.assertLess(time.monotonic(), deadline, "the test DLL was never deployed")
                time.sleep(0.01)
            process.send_signal(signal.SIGKILL)  # no trap runs: the test DLL and the plugin stay deployed
            process.wait(timeout=30)
        finally:
            if process.poll() is None:
                process.kill()

    def rerun(self, name="run2"):
        return subprocess.run([str(self.checkout / "verify-server.sh"), name, "default", "1"], env=self.env,
                              cwd=self.checkout, capture_output=True, text=True, timeout=60)

    def test_a_killed_run_is_restored_by_the_next_run_not_mistaken_for_the_original(self):
        # The fake wine's orphan (FAKE_WINE_SLEEP) outlives SIGKILL for 8 s; it must NOT hold the lock.
        self.kill_after_deploy(dict(self.env, FAKE_WINE_SLEEP="8"))
        self.assertEqual((self.win10 / "BugSplat64.dll").read_bytes(), TEST_DLL)
        waited = self.wait_for_lock_release()
        self.assertLess(waited, 5.0, "the orphaned game kept the run lock")
        # A rerun under ANOTHER name must find and restore the leftovers before deploying again.
        result = self.rerun()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("restoring files an earlier verify-server.sh run left deployed", result.stdout)
        self.assertEqual((self.win10 / "BugSplat64.dll").read_bytes(), ORIGINAL)
        self.assertFalse((self.win10 / "plugins/extra.dll").exists())

    def test_a_rerun_while_a_child_of_the_killed_run_is_still_working_is_refused_then_succeeds(self):
        # A slow cp stands for the child the killed shell had just started: it holds the lock descriptor.
        slow = self.tmp / "slowbin"
        slow.mkdir()
        mark = self.tmp / "cp-started"
        (slow / "cp").write_text('#!/bin/bash\necho started >> "$FAKE_CP_MARK"\nsleep "${FAKE_CP_DELAY:-0}"\nexec /bin/cp "$@"\n')
        (slow / "cp").chmod(0o755)
        env = dict(self.env, FAKE_CP_DELAY="1.5", FAKE_WINE_SLEEP="8", FAKE_CP_MARK=str(mark))
        env["PATH"] = f"{slow}:{env['PATH']}"
        # cp 1 saves the DLL, cp 2 deploys it, cp 3 deploys the plugin: kill while cp 3 is running and sleeping.
        self.kill_after_deploy(env, lambda: mark.exists() and len(mark.read_text().split()) >= 3)
        refused = self.rerun()
        self.assertEqual(refused.returncode, 4, refused.stdout + refused.stderr)
        self.assertIn("holds", refused.stderr)
        self.wait_for_lock_release()
        result = self.rerun("run3")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual((self.win10 / "BugSplat64.dll").read_bytes(), ORIGINAL)

    def test_a_refused_run_leaves_the_running_runs_log_alone(self):
        log = self.tmp / "server-runs/run1/server.log"
        log.parent.mkdir(parents=True)
        log.write_text("lines of the run that holds the lock\n")
        with open(self.tmp / "launch.lock", "w") as held:
            fcntl.flock(held, fcntl.LOCK_EX | fcntl.LOCK_NB)
            result = self.run_verify()
        self.assertEqual(result.returncode, 4, result.stdout + result.stderr)
        self.assertEqual(log.read_text(), "lines of the run that holds the lock\n")

    def test_a_failed_restore_is_loud_and_fails_the_run(self):
        result = self.run_verify(dict(self.env, FAKE_WINE_LOCK_DLL="1"))
        self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("ERROR: restoring", result.stderr)
        self.assertIn("ERROR: restoring", (self.tmp / "server-runs/run1/server.log").read_text())
        self.assertTrue((self.tmp / "verify-server-deploy-backup").exists(), "the backup must survive a failed restore")

    def test_refuses_to_deploy_while_echovr_is_running(self):
        env = dict(self.env, FAKE_ECHOVR_PIDS="4242")
        result = subprocess.run(self.command(), env=env, cwd=self.checkout, capture_output=True, text=True,
                                timeout=60)
        self.assertEqual(result.returncode, 4, result.stdout + result.stderr)
        self.assertIn("echovr.exe is already running (pid 4242)", result.stderr)
        self.assertEqual((self.win10 / "BugSplat64.dll").read_bytes(), ORIGINAL)

    def test_refuses_while_a_client_run_holds_the_lock(self):
        with open(self.tmp / "launch.lock", "w") as held:
            fcntl.flock(held, fcntl.LOCK_EX | fcntl.LOCK_NB)
            result = subprocess.run(self.command(), env=self.env, cwd=self.checkout, capture_output=True,
                                    text=True, timeout=60)
        self.assertEqual(result.returncode, 4, result.stdout + result.stderr)
        self.assertEqual((self.win10 / "BugSplat64.dll").read_bytes(), ORIGINAL)


class LaunchServerTest(unittest.TestCase):
    """launch-server.sh (#171): finds the game install without a local echovr/, and shares the run lock."""

    def setUp(self):
        self.tmp = pathlib.Path(tempfile.mkdtemp(prefix="launch-server-test-", dir="/var/tmp"))
        self.addCleanup(shutil.rmtree, self.tmp)
        self.game_root = make_game_root(self.tmp / "main")
        self.fake_bin = self.tmp / "bin"
        make_fake_bin(self.fake_bin)
        self.checkout = self.tmp / "checkout"
        self.checkout.mkdir()
        install_scripts(self.checkout)
        bin_dir = self.checkout / "build/mingw-release/bin"
        bin_dir.mkdir(parents=True)
        (bin_dir / "BugSplat64.dll").write_bytes(TEST_DLL)
        self.win10 = self.game_root / "echovr/bin/win10"
        self.env = dict(
            os.environ,
            PATH=f"{self.fake_bin}:{os.environ['PATH']}",
            NEVR_GAME_ROOT=str(self.game_root),
            NEVR_LAUNCH_LOCK=str(self.tmp / "launch.lock"),
            FAKE_EXPECT_DLL=str(bin_dir / "BugSplat64.dll"),
        )
        self.env.pop("FAKE_ECHOVR_PIDS", None)

    def run_script(self, env=None):
        return subprocess.run([str(self.checkout / "launch-server.sh")], env=env or self.env, cwd=self.checkout,
                              capture_output=True, text=True, timeout=60)

    def test_deploys_into_the_game_root_from_a_checkout_without_echovr(self):
        result = self.run_script()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual((self.win10 / "BugSplat64.dll").read_bytes(), TEST_DLL)
        self.assertFalse((self.checkout / "echovr").exists())

    def test_refuses_while_another_game_run_holds_the_lock(self):
        with open(self.tmp / "launch.lock", "w") as held:
            fcntl.flock(held, fcntl.LOCK_EX | fcntl.LOCK_NB)
            result = self.run_script()
        self.assertEqual(result.returncode, 4, result.stdout + result.stderr)
        self.assertEqual((self.win10 / "BugSplat64.dll").read_bytes(), ORIGINAL)

    def test_refuses_while_echovr_is_running(self):
        result = self.run_script(dict(self.env, FAKE_ECHOVR_PIDS="4242"))
        self.assertEqual(result.returncode, 4, result.stdout + result.stderr)
        self.assertIn("echovr.exe is already running (pid 4242)", result.stderr)
        self.assertEqual((self.win10 / "BugSplat64.dll").read_bytes(), ORIGINAL)

    def test_a_missing_game_install_is_a_clear_error(self):
        result = self.run_script(dict(self.env, NEVR_GAME_ROOT=str(self.tmp / "nowhere")))
        self.assertEqual(result.returncode, 2, result.stdout + result.stderr)
        self.assertIn("no game install", result.stderr)


if __name__ == "__main__":
    unittest.main()
