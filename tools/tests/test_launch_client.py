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
import select
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
TEST_RUNTIME = b"test-runtime"


def install_scripts(checkout: pathlib.Path) -> None:
    """Copy the scripts under test and the helper they source into a fake checkout."""
    (checkout / "tools/lib").mkdir(parents=True, exist_ok=True)
    shutil.copy(LIB, checkout / "tools/lib/game_install.sh")
    shutil.copy(LIB.with_name("work_root.sh"), checkout / "tools/lib/work_root.sh")
    for script in (SCRIPT, VERIFY_SERVER, LAUNCH_SERVER):
        shutil.copy(script, checkout / script.name)


class Fifo:
    """A named pipe a fake binary writes one line into when something happens, so a test blocks on the event
    instead of polling for its effect. A writer end held open here keeps the reader from seeing EOF between
    two writers."""

    def __init__(self, path: pathlib.Path):
        os.mkfifo(path)
        self.path = path
        self.reader = os.open(path, os.O_RDONLY | os.O_NONBLOCK)
        self.holder = os.open(path, os.O_WRONLY)
        self.lines = 0
        self.pending = b""

    def close(self) -> None:
        os.close(self.reader)
        os.close(self.holder)

    def wait_for(self, lines: int, timeout: float = 30.0) -> None:
        """Block until `lines` lines have arrived in total; fail if they do not within `timeout` seconds."""
        deadline = time.monotonic() + timeout
        while self.lines < lines:
            left = deadline - time.monotonic()
            if left <= 0 or not select.select([self.reader], [], [], left)[0]:
                raise AssertionError(f"{self.path.name}: {self.lines} of {lines} lines within {timeout} s")
            self.pending += os.read(self.reader, 4096)
            self.lines += self.pending.count(b"\n")
            self.pending = self.pending.rsplit(b"\n", 1)[-1]


def open_gate(path: pathlib.Path) -> None:
    """Let a fake binary blocked in `read < gate` go on (the open waits until that reader is there)."""
    fd = os.open(path, os.O_WRONLY)
    os.write(fd, b"go\n")
    os.close(fd)


def make_game_root(root: pathlib.Path) -> pathlib.Path:
    """A fake main checkout: echovr/bin/win10 with the original DLL, an empty _local and a log dir."""
    win10 = root / "echovr/bin/win10"
    win10.mkdir(parents=True)
    (win10 / "BugSplat64.dll").write_bytes(ORIGINAL)
    (root / "echovr/_local").mkdir()
    (root / "echovr/.wineprefix/drive_c/users" / getpass.getuser() / "AppData/Local/nevr/echovr/logs").mkdir(
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
        '  if [[ "$a" == "([X]vfb|[X]ephyr) :101" ]]; then exit 0; fi\n'
        'done\n'
        'matches_cmdline=0\n'
        'for a in "$@"; do [[ "$a" == "-f" ]] && matches_cmdline=1; done\n'
        'if [[ -n "${FAKE_ECHOVR_PIDS:-}" && $matches_cmdline == 1 ]]; then echo "$FAKE_ECHOVR_PIDS"; exit 0; fi\n'
        'exit 1\n')
    (directory / "wine").write_text(
        '#!/bin/bash\n'
        'logs="$WINEPREFIX/drive_c/users/$(id -un)/AppData/Local/nevr/echovr/logs"\n'
        '[[ -n "${FAKE_WINE_STARTED:-}" ]] && echo started > "$FAKE_WINE_STARTED"  # the script deploys before it starts the game\n'
        '[[ -n "${FAKE_HOST_BOOTSTRAP:-}" ]] && echo \'{"event":"host_attach"}\' > "$logs/nevr-bootstrap.jsonl"  # the host writes its record into the same folder\n'
        'sleep "${FAKE_WINE_LOG_DELAY:-0}"\n'
        'if [[ -n "${FAKE_LOG_TEXT:-}" ]]; then printf "%b" "$FAKE_LOG_TEXT" > "$logs/${FAKE_LOG_PREFIX:-nevr-fake-}$(date +%s%N).jsonl"\n'
        'else echo \'{"msg":"NetGame switching state (from logging in, to logged in)"}\' > "$logs/${FAKE_LOG_PREFIX:-nevr-fake-}$(date +%s%N).jsonl"; fi\n'
        '[[ -n "${FAKE_WINE_ARGS:-}" ]] && printf "%s\\n" "$*" > "$FAKE_WINE_ARGS"\n'
        'cmp -s "$FAKE_EXPECT_DLL" "./BugSplat64.dll" || { echo "wrong DLL deployed" >&2; exit 9; }\n'
        '[[ -z "${FAKE_EXPECT_RUNTIME:-}" ]] || cmp -s "$FAKE_EXPECT_RUNTIME" "./nevr-runtime.dll" || { echo "wrong runtime deployed" >&2; exit 9; }\n'
        '(sleep 3 >/dev/null 2>&1) &  # a leftover child, like a lingering wineserver, must not keep the lock\n'
        '# (its output goes nowhere: a child holding the test\'s stdout pipe would make subprocess.run wait for it)\n'
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
        self.runtime = self.tmp / "nevr-runtime.dll"
        self.runtime.write_bytes(TEST_RUNTIME)
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
            FAKE_EXPECT_RUNTIME=str(self.runtime),
        )
        self.env.pop("FAKE_ECHOVR_PIDS", None)

    def run_script(self, *args, env=None, cwd=None):
        return subprocess.run([str(self.checkout / "launch-client.sh"), *args], env=env or self.env,
                              cwd=cwd or self.checkout, capture_output=True, text=True, timeout=60)

    def deployed(self) -> bytes:
        return (self.game_root / "echovr/bin/win10/BugSplat64.dll").read_bytes()

    def test_trace_exports_is_passed_to_the_game_and_absent_otherwise(self):
        args_file = self.tmp / "wine-args.txt"
        env = dict(self.env, FAKE_WINE_ARGS=str(args_file))
        result = self.run_script("--dll", str(self.dll), "--trace-exports", "pnsrad,pnsovr", env=env)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("-trace-exports pnsrad,pnsovr", args_file.read_text())
        self.assertIn("tracing platform DLL exports: pnsrad,pnsovr", result.stdout)
        result = self.run_script("--dll", str(self.dll), env=env)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertNotIn("-trace-exports", args_file.read_text())

    def test_trace_exports_refuses_a_list_with_other_characters(self):
        result = self.run_script("--dll", str(self.dll), "--trace-exports", "pnsrad;rm -rf")
        self.assertEqual(result.returncode, 2, result.stdout + result.stderr)
        self.assertIn("--trace-exports takes a comma separated list", result.stderr)
        self.assertEqual(self.deployed(), ORIGINAL)

    def test_game_arg_reaches_the_game_and_is_absent_by_default(self):
        args_file = self.tmp / "wine-args.txt"
        env = dict(self.env, FAKE_WINE_ARGS=str(args_file))
        result = self.run_script("--dll", str(self.dll), "--game-arg", "-mp", env=env)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(args_file.read_text().strip(), "./echovr.exe -windowed -mp", result.stdout)
        self.assertIn("extra game arguments: -mp", result.stdout)
        result = self.run_script("--dll", str(self.dll), env=env)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(args_file.read_text().strip(), "./echovr.exe -windowed", result.stdout)

    def test_game_arg_needs_a_value(self):
        result = self.run_script("--dll", str(self.dll), "--game-arg")
        self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("--game-arg needs one game argument", result.stderr)
        self.assertEqual(self.deployed(), ORIGINAL)

    def test_runs_from_a_checkout_without_echovr_and_restores_the_original(self):
        result = self.run_script("--dll", str(self.dll))
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("PASS: client reached logged in", result.stdout)
        self.assertIn("original BugSplat64.dll restored and verified, nevr-runtime.dll removed", result.stdout)
        self.assertEqual(self.deployed(), ORIGINAL)
        self.assertFalse((self.game_root / "echovr/bin/win10/nevr-runtime.dll").exists())

    def test_the_runtime_beside_the_dll_is_deployed_for_the_run_and_a_named_one_wins(self):
        other = self.tmp / "other-runtime.dll"
        other.write_bytes(b"OTHER-RUNTIME")
        result = self.run_script("--dll", str(self.dll), "--runtime-dll", str(other),
                                 env=dict(self.env, FAKE_EXPECT_RUNTIME=str(other)))
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertFalse((self.game_root / "echovr/bin/win10/nevr-runtime.dll").exists())

    def test_a_missing_runtime_is_refused_before_anything_is_deployed(self):
        self.runtime.unlink()
        result = self.run_script("--dll", str(self.dll))
        self.assertEqual(result.returncode, 2, result.stdout + result.stderr)
        self.assertIn("nevr-runtime.dll does not exist", result.stderr)
        self.assertEqual(self.deployed(), ORIGINAL)

    def test_a_nevr_runtime_dll_already_in_the_game_directory_is_drift(self):
        (self.game_root / "echovr/bin/win10/nevr-runtime.dll").write_bytes(b"LEFTOVER")
        result = self.run_script("--dll", str(self.dll))
        self.assertEqual(result.returncode, 3, result.stdout + result.stderr)
        self.assertIn("nevr-runtime.dll exists (expected absent)", result.stderr)
        self.assertEqual((self.game_root / "echovr/bin/win10/nevr-runtime.dll").read_bytes(), b"LEFTOVER", "not deleted")
        self.assertEqual(self.deployed(), ORIGINAL)

    def test_a_config_yaml_already_in_the_nevr_root_is_drift(self):
        root = self.game_root / "echovr/.wineprefix/drive_c/users" / getpass.getuser() / "AppData/Local/nevr/echovr"
        (root / "config.yaml").write_text("services: {}\n")
        result = self.run_script("--dll", str(self.dll))
        self.assertEqual(result.returncode, 3, result.stdout + result.stderr)
        self.assertIn("config.yaml exists (expected absent)", result.stderr)
        self.assertEqual((root / "config.yaml").read_text(), "services: {}\n", "not deleted")
        self.assertEqual(self.deployed(), ORIGINAL)

    def test_the_run_log_is_read_from_the_nevr_root(self):
        result = self.run_script("--dll", str(self.dll))
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertRegex(result.stdout, r"=== game log: .*/AppData/Local/nevr/echovr/logs/nevr-fake-")

    def test_the_hosts_bootstrap_record_in_the_same_folder_is_not_the_run_log(self):
        # In a root that had no nevr-bootstrap.jsonl when the run began, the host's record is a new nevr-*.jsonl
        # and sorts after the run log; judging it would fail a run that logged in.
        # A real run log is named nevr-<timestamp>.jsonl, which sorts before nevr-bootstrap.jsonl.
        env = dict(self.env, FAKE_HOST_BOOTSTRAP="1", FAKE_LOG_PREFIX="nevr-2026-10-27T17-57-53.")
        result = self.run_script("--dll", str(self.dll), env=env)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertRegex(result.stdout, r"=== game log: .*/logs/nevr-2026-10-27T17-57-53\.")
        self.assertNotIn("nevr-bootstrap.jsonl", result.stdout)

    def test_require_in_game_fails_when_the_client_only_logs_in(self):
        # The default fake run reaches "to logged in" and stops; with --require-in-game the launcher
        # must refuse that as never entering multiplayer, not report the one-assertion-short PASS.
        result = self.run_script("--dll", str(self.dll), "--require-in-game")
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn("client never reached in game", result.stderr)
        self.assertEqual(self.deployed(), ORIGINAL)

    def test_require_in_game_passes_when_the_client_reaches_in_game(self):
        env = dict(self.env, FAKE_LOG_TEXT='{"msg":"NetGame switching state (from logging in, to logged in)"}\\n'
                                            '{"msg":"NetGame switching state (from ready for game, to in game)"}\\n')
        result = self.run_script("--dll", str(self.dll), "--require-in-game", env=env)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("PASS: client reached in game (multiplayer entry)", result.stdout)
        self.assertEqual(self.deployed(), ORIGINAL)

    def test_without_require_in_game_the_launcher_stays_at_logged_in(self):
        # The player default must not change: without the flag a login-only run is still a PASS.
        result = self.run_script("--dll", str(self.dll))
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("PASS: client reached logged in", result.stdout)
        self.assertNotIn("PASS: client reached in game", result.stdout)

    def run_until_login(self, log_text=None, *extra, timeout_flag=("--login-timeout", "45"), env_extra=None):
        env = dict(self.env, FAKE_WINE_SLEEP="3", NEVR_LOGIN_POLL_SECONDS="0.1")
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
        self.assertLess(elapsed, 2.5, "it waited for the game to exit instead of ending the run at the verdict")
        self.assertEqual(self.deployed(), ORIGINAL)

    def test_exit_after_login_fails_when_the_service_stays_unavailable(self):
        result, elapsed = self.run_until_login('{"msg":"Service is unavailable"}\\n' * 3)
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn("client never reached logged in", result.stderr)
        self.assertLess(elapsed, 2.5)
        self.assertEqual(self.deployed(), ORIGINAL)

    def test_exit_after_login_fails_at_the_deadline_when_nothing_happens(self):
        result, elapsed = self.run_until_login('{"msg":"noise"}\\n', timeout_flag=("--login-timeout", "1"),
                                               env_extra={"NEVR_LOGIN_MIN_SECONDS": "1", "FAKE_WINE_SLEEP": "30"})
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn("client never reached logged in", result.stderr)
        self.assertGreaterEqual(elapsed, 1.0)
        self.assertLess(elapsed, 6.0)
        self.assertEqual(self.deployed(), ORIGINAL)

    def test_the_deadline_is_not_short_when_the_run_starts_late_in_a_second(self):
        # Whole-second arithmetic once ended a 2 s deadline after 1.04 s when the script read its
        # start at x.99 (a 1 s deadline would end after 0.04 s). A `date +%s` whose clock reads x.99 the first time it is asked puts every run
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
        result, elapsed = self.run_until_login('{"msg":"noise"}\\n', timeout_flag=("--login-timeout", "1"),
                                               env_extra={"NEVR_LOGIN_MIN_SECONDS": "1", "FAKE_WINE_SLEEP": "30"})
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertGreaterEqual(elapsed, 1.0)

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
        self.assertLess(elapsed, 2.5)

    def test_a_dll_that_embeds_nothing_ends_the_run_without_waiting_for_the_deadline(self):
        result, elapsed = self.run_until_login('{"msg":"[NEVR.CONFIG] built-in defaults embedded in this build: (none)"}\\n')
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn("embeds no service endpoints", result.stderr)
        self.assertLess(elapsed, 2.5)

    def test_a_previous_runs_log_from_the_same_second_cannot_decide_the_verdict(self):
        logs = self.game_root / "echovr/.wineprefix/drive_c/users" / getpass.getuser() / "AppData/Local/nevr/echovr/logs"
        stale = logs / "nevr-2026-10-07T00-00-00.000.jsonl"
        stale.write_text('{"msg":"NetGame switching state (from logging in, to logged in)"}\n')
        # This run's own log has no login at all; the stale file must not be mistaken for it.
        result, _ = self.run_until_login('{"msg":"noise"}\\n', timeout_flag=("--login-timeout", "1"),
                                         env_extra={"NEVR_LOGIN_MIN_SECONDS": "1", "FAKE_WINE_SLEEP": "30",
                                                    "FAKE_WINE_LOG_DELAY": "0.3"})
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

    def test_the_scenario_runner_launches_the_client_in_multiplayer(self):
        """The runner must hand the game -mp. With -windowed alone the client sits in the menu and no
        step that waits for "in game" can pass (#674); the launcher itself keeps -windowed as its default."""
        import sys
        sys.path.insert(0, str(REPO / "tools/scenario"))
        import run_scenario
        args_file = self.tmp / "wine-args.txt"
        env = dict(self.env, FAKE_WINE_ARGS=str(args_file))
        out = self.tmp / "scenario-out"
        out.mkdir()
        old_env = {k: os.environ.get(k) for k in ("NEVR_GAME_ROOT", "NEVR_LAUNCH_LOCK",
                                                  "NEVR_RUN_SCRATCH_ROOT", "PATH", "FAKE_WINE_ARGS",
                                                  "NEVR_EVIDENCE_DELAY", "FAKE_EXPECT_DLL")}
        os.environ.update(env)
        try:
            run = run_scenario.Run({"name": "launch-args"}, self.dll, out)
            try:
                run.launch()
            finally:
                run.teardown()
        finally:
            for k, v in old_env.items():
                if v is None:
                    os.environ.pop(k, None)
                else:
                    os.environ[k] = v
            # wineprefix() caches: leave the next test the prefix it resolves for itself.
            run_scenario.wineprefix.cache_clear()
        self.assertEqual(args_file.read_text().strip(), "./echovr.exe -windowed -mp")

    def test_the_scenario_runner_hands_its_config_to_the_launcher(self):
        """`--config` reaches launch-client.sh and so the game's `-config`, which is how a suite runs with
        network.session_router set (#708); without it the game gets no -config."""
        import sys
        sys.path.insert(0, str(REPO / "tools/scenario"))
        import run_scenario
        config = self.tmp / "cfg" / "config.json"
        config.parent.mkdir()
        config.write_text("{}")
        args_file = self.tmp / "wine-args.txt"
        env = dict(self.env, FAKE_WINE_ARGS=str(args_file))
        out = self.tmp / "scenario-out"
        out.mkdir()
        old_env = {k: os.environ.get(k) for k in ("NEVR_GAME_ROOT", "NEVR_LAUNCH_LOCK",
                                                  "NEVR_RUN_SCRATCH_ROOT", "PATH", "FAKE_WINE_ARGS",
                                                  "NEVR_EVIDENCE_DELAY", "FAKE_EXPECT_DLL")}
        os.environ.update(env)
        try:
            run = run_scenario.Run({"name": "launch-config"}, self.dll, out, config)
            try:
                run.launch()
            finally:
                run.teardown()
        finally:
            for k, v in old_env.items():
                if v is None:
                    os.environ.pop(k, None)
                else:
                    os.environ[k] = v
            run_scenario.wineprefix.cache_clear()
        expected = "./echovr.exe -windowed -mp -config Z:" + str(config).replace("/", "\\")
        self.assertEqual(args_file.read_text().strip(), expected)


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
        (bin_dir / "nevr-runtime.dll").write_bytes(TEST_RUNTIME)
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
            FAKE_EXPECT_RUNTIME=str(bin_dir / "nevr-runtime.dll"),
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
        self.assertFalse((self.win10 / "nevr-runtime.dll").exists(), "the deployed runtime was left behind")
        self.assertIn("deployed files restored and verified", (self.tmp / "server-runs/run1/server.log").read_text())

    def test_the_timestep_flag_set_passes_the_options_a_server_strips(self):
        # #515: the run that shows GameMainWrapperHook removing -fixedtimestep and -timestep <value>.
        result = subprocess.run([str(self.checkout / "verify-server.sh"), "run-ts", "timestep", "1"], env=self.env,
                                cwd=self.checkout, capture_output=True, text=True, timeout=60)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("args=-server -headless -no-console -fixedtimestep -timestep 120",
                      (self.tmp / "server-runs/run-ts/context.txt").read_text().splitlines())

    def started_fifo(self) -> Fifo:
        fifo = Fifo(self.tmp / "game-started")
        self.addCleanup(fifo.close)
        return fifo

    def test_an_interrupted_run_still_restores(self):
        started = self.started_fifo()
        env = dict(self.env, FAKE_WINE_SLEEP="30", FAKE_WINE_STARTED=str(started.path))
        process = subprocess.Popen(self.command(), env=env, cwd=self.checkout, stdout=subprocess.PIPE,
                                   stderr=subprocess.STDOUT, text=True)
        try:
            started.wait_for(1)  # the script deploys before it starts the game
            self.assertEqual((self.win10 / "BugSplat64.dll").read_bytes(), TEST_DLL)
            process.send_signal(signal.SIGTERM)
            process.wait(timeout=30)
        finally:
            if process.poll() is None:
                process.kill()
            process.stdout.close()
        self.assertEqual((self.win10 / "BugSplat64.dll").read_bytes(), ORIGINAL)
        self.assertFalse((self.win10 / "plugins/extra.dll").exists())

    def test_an_interrupt_ends_the_census_wait_at_once(self):
        # A sample interval of 30 s: the SIGTERM has to run the restore now, not when the interval ends.
        started = self.started_fifo()
        env = dict(self.env, FAKE_WINE_SLEEP="30", FAKE_WINE_STARTED=str(started.path), NEVR_VERIFY_POLL_SECONDS="30")
        process = subprocess.Popen([str(self.checkout / "verify-server.sh"), "run1", "default", "60"], env=env,
                                   cwd=self.checkout, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        try:
            started.wait_for(1)
            asked = time.monotonic()
            process.send_signal(signal.SIGTERM)
            process.wait(timeout=10)
            self.assertLess(time.monotonic() - asked, 3.0)
        finally:
            if process.poll() is None:
                process.kill()
        self.assertEqual((self.win10 / "BugSplat64.dll").read_bytes(), ORIGINAL)
        self.assertFalse((self.win10 / "plugins/extra.dll").exists())

    def test_a_game_that_exits_at_once_ends_the_census_at_once(self):
        # The fake game exits immediately; a sample interval of 30 s must not be waited out.
        started = time.monotonic()
        result = self.run_verify(dict(self.env, NEVR_VERIFY_POLL_SECONDS="30"))
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertLess(time.monotonic() - started, 5.0)
        self.assertIn("process exited early at t=", (self.tmp / "server-runs/run1/context.txt").read_text())

    def test_a_game_that_stays_up_is_sampled_each_interval(self):
        # Time is what this test is about: one interval of 1 s with the game alive for it.
        result = self.run_verify(dict(self.env, FAKE_WINE_SLEEP="1.5"))
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        run = self.tmp / "server-runs/run1"
        self.assertRegex((run / "window-census.txt").read_text(), r"^t=1s game_pids=\[")
        self.assertNotIn("process exited early", (run / "context.txt").read_text())
        self.assertIn("max_game_windows=", (run / "context.txt").read_text())

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
        descriptor, keeps the lock until it ends; a rerun in that moment is refused, correctly (#295).
        `flock -w` blocks until the last holder ends, so nothing here polls."""
        started = time.monotonic()
        taken = subprocess.run(["flock", "-w", str(timeout), str(self.tmp / "launch.lock"), "true"])
        self.assertEqual(taken.returncode, 0, "the killed run's lock was never released")
        return time.monotonic() - started

    def kill_after(self, env, wait_for):
        """Start a run, block in `wait_for()` until the event the test names has happened, then SIGKILL it."""
        process = subprocess.Popen(self.command(), env=env, cwd=self.checkout, stdout=subprocess.DEVNULL,
                                   stderr=subprocess.DEVNULL)
        try:
            wait_for()
            process.send_signal(signal.SIGKILL)  # no trap runs: the test DLL and the plugin stay deployed
            process.wait(timeout=30)
        finally:
            if process.poll() is None:
                process.kill()

    def rerun(self, name="run2"):
        return subprocess.run([str(self.checkout / "verify-server.sh"), name, "default", "1"], env=self.env,
                              cwd=self.checkout, capture_output=True, text=True, timeout=60)

    def test_the_backup_a_killed_run_leaves_in_the_work_root_says_what_it_is(self):
        # With no NEVR_RUN_SCRATCH_ROOT the backup is a directory in the work root, and carries a README.
        work = self.tmp / "work"
        env = {k: v for k, v in self.env.items() if k != "NEVR_RUN_SCRATCH_ROOT"}
        started = self.started_fifo()
        self.kill_after(dict(env, NEVR_WORK_ROOT=str(work), FAKE_WINE_SLEEP="8", FAKE_WINE_STARTED=str(started.path)),
                        lambda: started.wait_for(1))
        readme = (work / "verify-server-deploy-backup/README.md").read_text()
        self.assertIn("saved from the game install", readme)
        self.wait_for_lock_release()

    def test_a_killed_run_is_restored_by_the_next_run_not_mistaken_for_the_original(self):
        # The fake wine's orphan (FAKE_WINE_SLEEP) outlives SIGKILL for 8 s; it must NOT hold the lock.
        started = self.started_fifo()
        self.kill_after(dict(self.env, FAKE_WINE_SLEEP="8", FAKE_WINE_STARTED=str(started.path)),
                        lambda: started.wait_for(1))
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
        # A cp that waits at a gate stands for the child the killed shell had just started: it holds the lock
        # descriptor until the test opens the gate.
        slow = self.tmp / "slowbin"
        slow.mkdir()
        cp_started = Fifo(self.tmp / "cp-started")
        self.addCleanup(cp_started.close)
        gate = self.tmp / "cp-gate"
        os.mkfifo(gate)
        (slow / "cp").write_text(
            '#!/bin/bash\n'
            'echo started > "$FAKE_CP_STARTED"\n'
            'n=$(( $(cat "$FAKE_CP_COUNT" 2>/dev/null || echo 0) + 1 )); echo $n > "$FAKE_CP_COUNT"\n'
            '[[ $n -eq 3 ]] && read -r _ < "$FAKE_CP_GATE"\n'
            'exec /bin/cp "$@"\n')
        (slow / "cp").chmod(0o755)
        env = dict(self.env, FAKE_WINE_SLEEP="8", FAKE_CP_STARTED=str(cp_started.path),
                   FAKE_CP_COUNT=str(self.tmp / "cp-count"), FAKE_CP_GATE=str(gate))
        env["PATH"] = f"{slow}:{env['PATH']}"
        # cp 1 saves the DLL, cp 2 deploys it, cp 3 deploys the plugin: kill while cp 3 waits at the gate.
        self.kill_after(env, lambda: cp_started.wait_for(3))
        refused = self.rerun()
        self.assertEqual(refused.returncode, 4, refused.stdout + refused.stderr)
        self.assertIn("holds", refused.stderr)
        open_gate(gate)
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
        (bin_dir / "nevr-runtime.dll").write_bytes(TEST_RUNTIME)
        self.win10 = self.game_root / "echovr/bin/win10"
        self.env = dict(
            os.environ,
            PATH=f"{self.fake_bin}:{os.environ['PATH']}",
            NEVR_GAME_ROOT=str(self.game_root),
            NEVR_LAUNCH_LOCK=str(self.tmp / "launch.lock"),
            FAKE_EXPECT_DLL=str(bin_dir / "BugSplat64.dll"),
            FAKE_EXPECT_RUNTIME=str(bin_dir / "nevr-runtime.dll"),
        )
        self.env.pop("FAKE_ECHOVR_PIDS", None)

    def run_script(self, env=None):
        return subprocess.run([str(self.checkout / "launch-server.sh")], env=env or self.env, cwd=self.checkout,
                              capture_output=True, text=True, timeout=60)

    def test_deploys_into_the_game_root_from_a_checkout_without_echovr(self):
        started = time.monotonic()
        result = self.run_script()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertLess(time.monotonic() - started, 2.0, "the run waited for the game's leftover child")
        self.assertEqual((self.win10 / "BugSplat64.dll").read_bytes(), TEST_DLL)
        self.assertEqual((self.win10 / "nevr-runtime.dll").read_bytes(), TEST_RUNTIME)
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
