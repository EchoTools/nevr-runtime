"""launch-client.sh and verify-server.sh: where they find the game, that only one game run happens
at a time, and that verify-server.sh puts back what it deployed.

The script runs against a fake game install and fake `wine`/`pgrep`/`wineserver`/`Xephyr` on PATH,
so these tests never start the real game. NEVR_EVIDENCE_DELAY=0 skips the script's 12 s wait.
"""
from __future__ import annotations

import fcntl
import getpass
import json
import os
import pathlib
import re
import shutil
import stat
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


def game_standin(testcase: unittest.TestCase) -> int:
    """A live process whose argv[0] is ./echovr.exe, so game_pids (tools/lib/game_install.sh) counts it as the game
    (#1490). Its pid is what the fake pgrep reports as FAKE_ECHOVR_PIDS. Killed when the test ends."""
    proc = subprocess.Popen(["bash", "-c", "exec -a ./echovr.exe sleep 600"], start_new_session=True)
    testcase.addCleanup(lambda: (proc.kill(), proc.wait()))
    deadline = time.monotonic() + 5
    while time.monotonic() < deadline:
        with open(f"/proc/{proc.pid}/cmdline", "rb") as f:
            if f.read().startswith(b"./echovr.exe\0"):
                return proc.pid
        time.sleep(0.02)
    raise AssertionError(f"stand-in pid {proc.pid} never exec'd")
TEST_RUNTIME = b"test-runtime"
SIGNIN = b'{"refresh_token":"SENTINEL-CRED-MUST-NOT-PRINT"}'
SENTINEL = "SENTINEL-CRED-MUST-NOT-PRINT"
ROTATED = '{"refresh_token":"ROTATED-SENTINEL-MUST-NOT-PRINT"}'
ROTATED_SENTINEL = "ROTATED-SENTINEL-MUST-NOT-PRINT"
ACCOUNT = "test-account"
LINK_MARKER = "FIXTURE-SIGNIN-THROUGH-A-LINK"
# The real Wine, for the one test that checks the URL chain itself (skipped when Wine is not installed).
REAL_WINE = shutil.which("wine")
REAL_WINESERVER = shutil.which("wineserver")


def install_scripts(checkout: pathlib.Path) -> None:
    """Copy the scripts under test and the helper they source into a fake checkout."""
    (checkout / "tools/lib").mkdir(parents=True, exist_ok=True)
    shutil.copy(LIB, checkout / "tools/lib/game_install.sh")
    shutil.copy(LIB.with_name("work_root.sh"), checkout / "tools/lib/work_root.sh")
    shutil.copy(REPO / "tools/host_stop_cause.py", checkout / "tools/host_stop_cause.py")
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


def nevr_root_of(game_root: pathlib.Path) -> pathlib.Path:
    """The nevr root of the prefix's Windows user: where the host looks for nevr-runtime.dll (ADR 0012)."""
    return game_root / "echovr/.wineprefix/drive_c/users" / getpass.getuser() / "AppData/Local/nevr/echovr"


def plant_store(work: pathlib.Path, account: str = ACCOUNT, content: bytes = SIGNIN, mode: int = 0o600) -> pathlib.Path:
    """A stored sign-in as the launchers keep it: $NEVR_WORK_ROOT/creds/<account>/.credentials.json."""
    path = work / "creds" / account / ".credentials.json"
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(content)
    path.chmod(mode)
    return path


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
        '# A host that refuses nevr-runtime.dll writes these records and the runtime, which writes the game log, never runs.\n'
        'if [[ -n "${FAKE_HOST_STOP:-}" ]]; then\n'
        '  ts=$(date -u +%Y-%m-%dT%H:%M:%S.000Z)\n'
        '  printf \'%s\\n\' \'{"event":"runtime_load","outcome":"runtime_refused","path":"Z:\\\\game\\\\nevr-runtime.dll","run":"r1","tag":"NEVR.HOST","ts":"\'$ts\'","verify":"refused","verify_reason":"\'$FAKE_HOST_STOP\'"}\' \\\n'
        '    \'{"event":"game_stop","exit_code":78,"reason":"runtime_refused","run":"r1","tag":"NEVR.HOST","ts":"\'$ts\'"}\' > "$logs/nevr-bootstrap.jsonl"\n'
        'elif [[ -n "${FAKE_LOG_TEXT:-}" ]]; then sleep "${FAKE_WINE_LOG_DELAY:-0}"; printf "%b" "$FAKE_LOG_TEXT" > "$logs/${FAKE_LOG_PREFIX:-nevr-fake-}$(date +%s%N).jsonl"\n'
        'else sleep "${FAKE_WINE_LOG_DELAY:-0}"; echo \'{"msg":"NetGame switching state (from logging in, to logged in)"}\' > "$logs/${FAKE_LOG_PREFIX:-nevr-fake-}$(date +%s%N).jsonl"; fi\n'
        '[[ -n "${FAKE_WINE_ARGS:-}" ]] && printf "%s\\n" "$*" > "$FAKE_WINE_ARGS"\n'
        'cmp -s "$FAKE_EXPECT_DLL" "./BugSplat64.dll" || { echo "wrong DLL deployed" >&2; exit 9; }\n'
        '# The host loads the runtime from the nevr root (the folder above logs), never from beside itself (ADR 0012).\n'
        '[[ -z "${FAKE_EXPECT_RUNTIME:-}" ]] || cmp -s "$FAKE_EXPECT_RUNTIME" "$logs/../nevr-runtime.dll" || { echo "wrong runtime deployed" >&2; exit 9; }\n'
        '[[ -z "${FAKE_EXPECT_RUNTIME:-}" ]] || [[ ! -e "./nevr-runtime.dll" ]] || { echo "runtime deployed beside the host" >&2; exit 9; }\n'
        '# A plugin the run was given is in the nevr root\'s plugins folder while the game runs (ADR 0012).\n'
        '[[ -z "${FAKE_EXPECT_PLUGIN:-}" ]] || cmp -s "$FAKE_EXPECT_PLUGIN" "$logs/../plugins/$(basename "$FAKE_EXPECT_PLUGIN")" || { echo "wrong plugin deployed" >&2; exit 9; }\n'
        '(sleep 3 >/dev/null 2>&1) &  # a leftover child, like a lingering wineserver, must not keep the lock\n'
        '# (its output goes nowhere: a child holding the test\'s stdout pipe would make subprocess.run wait for it)\n'
        '# A directory where the DLL was makes the restore fail for any user; chmod 444 does not stop root,\n'
        '# which is who runs the tests in the CI container.\n'
        '[[ -n "${FAKE_WINE_LOCK_DLL:-}" ]] && { rm -f ./BugSplat64.dll; mkdir ./BugSplat64.dll; }\n'
        '# What the sign-in and browser checks read: the link while the game runs, a sign-in the runtime saves, the browser.\n'
        '[[ -n "${FAKE_WINE_LINK:-}" ]] && { if [[ -L "$logs/../.credentials.json" ]]; then readlink "$logs/../.credentials.json" > "$FAKE_WINE_LINK"; else echo absent > "$FAKE_WINE_LINK"; fi; }\n'
        '[[ -n "${FAKE_WINE_WRITE_SIGNIN:-}" ]] && printf "%s" "$FAKE_WINE_WRITE_SIGNIN" > "$logs/../.credentials.json"\n'
        '# A refreshing runtime replaces the link with a regular file of its own (#1469).\n'
        '[[ -n "${FAKE_WINE_REPLACE_SIGNIN:-}" ]] && { rm -f "$logs/../.credentials.json"; printf "%s" "$FAKE_WINE_REPLACE_SIGNIN" > "$logs/../.credentials.json"; }\n'
        '[[ -n "${FAKE_WINE_REPOINT:-}" ]] && ln -sfn -- "$FAKE_WINE_REPOINT" "$logs/../.credentials.json"\n'
        '[[ -n "${FAKE_WINE_XDG:-}" ]] && command -v xdg-open > "$FAKE_WINE_XDG"\n'
        '[[ -n "${FAKE_WINE_IGNORE_INT:-}" ]] && trap "" INT  # a client that ignores the interrupt: timeout must KILL it\n'
        '# A directory where the run\'s sign-in link is: rm cannot remove it, so the restore must say the sign-in is still there.\n'
        '[[ -n "${FAKE_WINE_PIN_SIGNIN:-}" ]] && { rm -f "$logs/../.credentials.json"; mkdir "$logs/../.credentials.json"; }\n'
        # A server run writes the boot lines a healthy run writes, to the capture verify-server.sh reads (the game's output).
        '[[ "$*" == *-server* && -z "${FAKE_HOST_STOP:-}" && -z "${FAKE_WINE_NO_BOOT:-}" ]] && printf "%s\\n" "Finished initializing engine"\n'
        '[[ "$*" == *-server* && -z "${FAKE_HOST_STOP:-}" && -z "${FAKE_WINE_NO_BOOT:-}" && -z "${FAKE_WINE_NO_LOGIN:-}" ]] && printf "%s\\n" "LOGIN SUCCESS" "registration successful"\n'
        # A game that exits with a code of its own (FAKE_WINE_EXIT=78 is the host stop's exit code).
        '[[ -n "${FAKE_WINE_EXIT:-}" ]] && exit "$FAKE_WINE_EXIT"\n'
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
        self.work = self.tmp / "work"
        self.store = plant_store(self.work)
        self.env = dict(
            os.environ,
            PATH=f"{self.fake_bin}:{os.environ['PATH']}",
            NEVR_GAME_ROOT=str(self.game_root),
            NEVR_LAUNCH_LOCK=str(self.lock),
            NEVR_EVIDENCE_DELAY="0",
            NEVR_RUN_SCRATCH_ROOT=str(self.tmp),
            NEVR_WORK_ROOT=str(self.work),
            FAKE_EXPECT_DLL=str(self.dll),
            FAKE_EXPECT_RUNTIME=str(self.runtime),
        )
        self.env.pop("FAKE_ECHOVR_PIDS", None)

    def run_script(self, *args, env=None, cwd=None, auth=("--creds", ACCOUNT)):
        """Run launch-client.sh. A test client signs in with the stored account (the owner's rule) unless the arguments
        already name a sign-in; auth=() runs with none, for the refusals and the device-code runs."""
        if auth and not {"--creds", "--device-code"} & set(args):
            args = (*args, *auth)
        return subprocess.run([str(self.checkout / "launch-client.sh"), *args], env=env or self.env,
                              cwd=cwd or self.checkout, capture_output=True, text=True, timeout=60)

    def deployed(self) -> bytes:
        return (self.game_root / "echovr/bin/win10/BugSplat64.dll").read_bytes()

    @property
    def root_runtime(self) -> pathlib.Path:
        return nevr_root_of(self.game_root) / "nevr-runtime.dll"

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
        # auth=(): the harness would append --creds, which --game-arg would take as its value.
        result = self.run_script("--dll", str(self.dll), "--game-arg", auth=())
        self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("--game-arg needs one game argument", result.stderr)
        self.assertEqual(self.deployed(), ORIGINAL)

    def test_runs_from_a_checkout_without_echovr_and_restores_the_original(self):
        result = self.run_script("--dll", str(self.dll))
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("PASS: client reached logged in", result.stdout)
        self.assertIn("original BugSplat64.dll restored and verified, nevr-runtime.dll removed from the nevr root", result.stdout)
        self.assertEqual(self.deployed(), ORIGINAL)
        self.assertFalse((self.game_root / "echovr/bin/win10/nevr-runtime.dll").exists())
        self.assertFalse(self.root_runtime.exists(), "the deployed runtime was left in the nevr root")

    def test_the_runtime_beside_the_dll_is_deployed_for_the_run_and_a_named_one_wins(self):
        other = self.tmp / "other-runtime.dll"
        other.write_bytes(b"OTHER-RUNTIME")
        result = self.run_script("--dll", str(self.dll), "--runtime-dll", str(other),
                                 env=dict(self.env, FAKE_EXPECT_RUNTIME=str(other)))
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertFalse((self.game_root / "echovr/bin/win10/nevr-runtime.dll").exists())
        self.assertFalse(self.root_runtime.exists())

    def test_a_missing_runtime_is_refused_before_anything_is_deployed(self):
        self.runtime.unlink()
        result = self.run_script("--dll", str(self.dll))
        self.assertEqual(result.returncode, 2, result.stdout + result.stderr)
        self.assertIn("nevr-runtime.dll does not exist", result.stderr)
        self.assertEqual(self.deployed(), ORIGINAL)

    def test_a_nevr_runtime_dll_already_in_the_nevr_root_is_drift(self):
        self.root_runtime.parent.mkdir(parents=True, exist_ok=True)
        self.root_runtime.write_bytes(b"LEFTOVER")
        result = self.run_script("--dll", str(self.dll))
        self.assertEqual(result.returncode, 3, result.stdout + result.stderr)
        self.assertIn(f"{self.root_runtime} exists (expected absent)", result.stderr)
        self.assertEqual(self.root_runtime.read_bytes(), b"LEFTOVER", "not deleted")
        self.assertEqual(self.deployed(), ORIGINAL)

    def test_a_nevr_runtime_dll_already_in_the_game_directory_is_drift(self):
        (self.game_root / "echovr/bin/win10/nevr-runtime.dll").write_bytes(b"LEFTOVER")
        result = self.run_script("--dll", str(self.dll))
        self.assertEqual(result.returncode, 3, result.stdout + result.stderr)
        self.assertIn("nevr-runtime.dll exists (expected absent)", result.stderr)
        self.assertEqual((self.game_root / "echovr/bin/win10/nevr-runtime.dll").read_bytes(), b"LEFTOVER", "not deleted")
        self.assertEqual(self.deployed(), ORIGINAL)

    def test_a_named_plugin_is_in_the_nevr_roots_plugins_folder_for_the_run_and_gone_after(self):
        plugin = self.tmp / "my_plugin.dll"
        plugin.write_bytes(b"A-PLUGIN")
        result = self.run_script("--dll", str(self.dll), "--plugin", str(plugin),
                                 env=dict(self.env, FAKE_EXPECT_PLUGIN=str(plugin)))
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("my_plugin.dll", result.stdout)
        self.assertFalse((nevr_root_of(self.game_root) / "plugins" / "my_plugin.dll").exists(), "the run's plugin is removed")
        self.assertFalse((self.game_root / "echovr/bin/win10/plugins").exists(), "nothing goes in the game folder")

    def test_a_plugin_that_does_not_exist_is_refused_before_anything_is_deployed(self):
        result = self.run_script("--dll", str(self.dll), "--plugin", str(self.tmp / "missing.dll"))
        self.assertEqual(result.returncode, 2, result.stdout + result.stderr)
        self.assertIn("missing.dll does not exist", result.stderr)
        self.assertEqual(self.deployed(), ORIGINAL)

    def test_a_plugin_in_the_nevr_roots_plugins_folder_is_drift(self):
        plugins = self.root_runtime.parent / "plugins"
        plugins.mkdir(parents=True, exist_ok=True)
        (plugins / "leftover.dll").write_bytes(b"LEFTOVER")
        result = self.run_script("--dll", str(self.dll))
        self.assertEqual(result.returncode, 3, result.stdout + result.stderr)
        self.assertIn("plugins folder is not empty", result.stderr)
        self.assertIn(str(plugins / "leftover.dll"), result.stderr)
        self.assertEqual((plugins / "leftover.dll").read_bytes(), b"LEFTOVER", "not deleted")
        self.assertEqual(self.deployed(), ORIGINAL)

    def test_a_plugin_in_the_game_directorys_plugins_folder_is_drift(self):
        plugins = self.game_root / "echovr/bin/win10/plugins"
        plugins.mkdir(parents=True, exist_ok=True)
        (plugins / "leftover.dll").write_bytes(b"LEFTOVER")
        result = self.run_script("--dll", str(self.dll))
        self.assertEqual(result.returncode, 3, result.stdout + result.stderr)
        self.assertIn(str(plugins / "leftover.dll"), result.stderr)
        self.assertEqual(self.deployed(), ORIGINAL)

    def test_the_run_log_is_read_from_the_nevr_root(self):
        result = self.run_script("--dll", str(self.dll))
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertRegex(result.stdout, r"=== game log: .*/AppData/Local/nevr/echovr/logs/nevr-fake-")

    def test_the_run_log_is_copied_into_the_run_directory_before_the_game_copy_goes(self):
        # #1297: the test that made the game copy deletes it when the run ends, so the run's log must already be out of it.
        result = self.run_script("--dll", str(self.dll))
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        copies = list(self.tmp.glob("client-run-*/game-log.jsonl"))
        self.assertEqual(len(copies), 1, result.stdout)
        self.assertIn("to logged in", copies[0].read_text())
        self.assertRegex(result.stdout, r"=== game log copied to: .*/game-log\.jsonl ===")

    def test_a_run_log_that_cannot_be_copied_out_fails_the_run(self):
        # A cp that fails for the run log makes the copy fail: the run must fail loudly, not pass without its log.
        bindir = self.bin_with_failing("cp", '[[ "$*" == *game-log.jsonl* ]]')
        env = dict(self.env, PATH=f"{bindir}:{self.env['PATH']}")
        result = self.run_script("--dll", str(self.dll), env=env)
        self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("could not copy the game log", result.stderr)

    def test_the_hosts_bootstrap_record_in_the_same_folder_is_not_the_run_log(self):
        # In a root that had no nevr-bootstrap.jsonl when the run began, the host's record is a new nevr-*.jsonl
        # and sorts after the run log; judging it would fail a run that logged in.
        # A real run log is named nevr-<timestamp>.jsonl, which sorts before nevr-bootstrap.jsonl.
        env = dict(self.env, FAKE_HOST_BOOTSTRAP="1", FAKE_LOG_PREFIX="nevr-2026-10-27T17-57-53.")
        result = self.run_script("--dll", str(self.dll), env=env)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertRegex(result.stdout, r"=== game log: .*/logs/nevr-2026-10-27T17-57-53\.")
        self.assertNotIn("nevr-bootstrap.jsonl", result.stdout)

    def test_a_host_that_refused_the_runtime_is_named_when_no_game_log_appears(self):
        # #1122: the host stops the game and the runtime, which writes the game log, never runs. The launcher says
        # why from the host's record, not only that no log was written.
        env = dict(self.env, FAKE_HOST_STOP="unsigned")
        result = self.run_script("--dll", str(self.dll), env=env)
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn("no game log written", result.stderr)
        self.assertIn("the host refused nevr-runtime.dll: unsigned", result.stderr)
        self.assertIn("build with mingw-plugin-dev or mingw-scenario", result.stderr)
        self.assertEqual(self.deployed(), ORIGINAL)

    def test_a_run_without_a_game_log_and_without_a_host_stop_keeps_the_plain_message(self):
        env = dict(self.env, FAKE_HOST_BOOTSTRAP="1", FAKE_LOG_TEXT="")
        env["FAKE_LOG_PREFIX"] = "other-"  # not a nevr-*.jsonl, so the launcher finds no game log
        result = self.run_script("--dll", str(self.dll), env=env)
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn("no game log written", result.stderr)
        self.assertNotIn("the host stopped the game", result.stderr)

    def test_a_stop_from_before_the_run_is_not_blamed_on_it(self):
        logs = self.game_root / "echovr/.wineprefix/drive_c/users" / getpass.getuser() / "AppData/Local/nevr/echovr/logs"
        (logs / "nevr-bootstrap.jsonl").write_text(
            '{"event":"game_stop","exit_code":78,"reason":"runtime_refused","run":"old","ts":"2026-10-01T00:00:00.000Z"}\n')
        env = dict(self.env, FAKE_LOG_PREFIX="other-")
        result = self.run_script("--dll", str(self.dll), env=env)
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertNotIn("the host stopped the game", result.stderr)

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
        pid = game_standin(self)
        env = dict(self.env, FAKE_ECHOVR_PIDS=str(pid))
        result = self.run_script("--dll", str(self.dll), env=env)
        self.assertEqual(result.returncode, 4, result.stdout + result.stderr)
        self.assertIn(f"echovr.exe is already running (pid {pid})", result.stderr)
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
                                                  "NEVR_EVIDENCE_DELAY", "FAKE_EXPECT_DLL", "NEVR_WORK_ROOT")}
        os.environ.update(env)
        try:
            run = run_scenario.Run({"name": "launch-args"}, self.dll, out, creds=ACCOUNT)
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
                                                  "NEVR_EVIDENCE_DELAY", "FAKE_EXPECT_DLL", "NEVR_WORK_ROOT")}
        os.environ.update(env)
        try:
            run = run_scenario.Run({"name": "launch-config"}, self.dll, out, config, creds=ACCOUNT)
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

    # --- the sign-in a test client uses (#1449) and the host browser it must not reach (#1447) ---

    def link_path(self) -> pathlib.Path:
        return nevr_root_of(self.game_root) / ".credentials.json"

    def test_a_run_with_no_sign_in_flag_is_refused_before_anything_is_touched(self):
        result = self.run_script("--dll", str(self.dll), auth=())
        self.assertEqual(result.returncode, 3, result.stdout + result.stderr)
        self.assertIn("pass --creds <account>", result.stderr)
        self.assertIn("--device-code", result.stderr)
        self.assertEqual(self.deployed(), ORIGINAL)
        self.assertFalse(self.lock.exists(), "the run lock was taken before the refusal")
        self.assertFalse(os.path.lexists(self.link_path()))

    def test_creds_for_an_account_with_no_stored_sign_in_names_the_first_run(self):
        result = self.run_script("--dll", str(self.dll), "--creds", "nobody", auth=())
        self.assertEqual(result.returncode, 3, result.stdout + result.stderr)
        self.assertIn("no cached credentials for nobody: run once with --device-code and --save-creds nobody",
                      result.stderr)
        self.assertEqual(self.deployed(), ORIGINAL)
        self.assertFalse(self.lock.exists(), "the run lock was taken before the refusal")

    def test_a_stored_sign_in_other_users_can_read_is_refused_without_printing_it(self):
        self.store.chmod(0o644)
        result = self.run_script("--dll", str(self.dll))
        self.assertEqual(result.returncode, 3, result.stdout + result.stderr)
        self.assertIn(str(self.store), result.stderr)
        self.assertIn("0644", result.stderr)
        self.assertNotIn(SENTINEL, result.stdout + result.stderr)
        self.assertEqual(self.deployed(), ORIGINAL)
        self.assertFalse(os.path.lexists(self.link_path()))

    def test_a_relative_work_root_is_refused_before_a_link_would_dangle(self):
        # The sign-in link's target must be absolute: a relative one resolves against the link's own folder.
        result = self.run_script("--dll", str(self.dll), env=dict(self.env, NEVR_WORK_ROOT="relative/work"))
        self.assertEqual(result.returncode, 2, result.stdout + result.stderr)
        self.assertIn("NEVR_WORK_ROOT must be an absolute path", result.stderr)
        self.assertEqual(self.deployed(), ORIGINAL)
        self.assertFalse(self.lock.exists(), "the run lock was taken before the refusal")

    def test_an_account_name_that_is_not_a_plain_name_is_refused(self):
        for bad in ("../elsewhere", "a/b", ".hidden", ""):
            with self.subTest(account=bad):
                result = self.run_script("--dll", str(self.dll), "--creds", bad, auth=())
                self.assertEqual(result.returncode, 2, result.stdout + result.stderr)
        self.assertEqual(self.deployed(), ORIGINAL)

    def test_an_install_that_holds_a_sign_in_takes_no_new_one(self):
        # NEVR_GAME_ROOT unset: the game root is the main checkout and its echovr/ is the real install, which holds
        # a sign-in. Neither a link nor a device-code login may join it: both runs are refused (a copy named by
        # NEVR_GAME_ROOT is refused by test_copy_credentials_guard.py, #1198).
        make_game_root(self.checkout)
        root = self.checkout / "echovr/.wineprefix/drive_c/users" / getpass.getuser() / "AppData/Local/nevr/echovr"
        root.mkdir(parents=True, exist_ok=True)
        (root / ".credentials.json").write_bytes(SIGNIN)
        env = {k: v for k, v in self.env.items() if k != "NEVR_GAME_ROOT"}
        for flags in (("--creds", ACCOUNT), ("--device-code",)):
            with self.subTest(flags=flags):
                result = self.run_script("--dll", str(self.dll), *flags, env=env, auth=())
                self.assertEqual(result.returncode, 3, result.stdout + result.stderr)
                self.assertIn(str(root / ".credentials.json"), result.stderr)
                self.assertNotIn(SENTINEL, result.stdout + result.stderr)
                self.assertEqual((root / ".credentials.json").read_bytes(), SIGNIN)
                self.assertEqual((self.checkout / "echovr/bin/win10/BugSplat64.dll").read_bytes(), ORIGINAL)

    def test_a_creds_run_links_the_stored_sign_in_into_the_nevr_root_for_the_run_only(self):
        seen = self.tmp / "link-seen"
        result = self.run_script("--dll", str(self.dll), "--creds", ACCOUNT,
                                 env=dict(self.env, FAKE_WINE_LINK=str(seen)))
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(seen.read_text().strip(), str(self.store),
                         "the game did not find the stored sign-in in the nevr root")
        self.assertFalse(os.path.lexists(self.link_path()), "the link was left in the copy")
        self.assertEqual(self.store.read_bytes(), SIGNIN, "the stored sign-in changed")
        self.assertEqual(self.store.stat().st_mode & 0o777, 0o600)
        self.assertNotIn(SENTINEL, result.stdout + result.stderr)

    def test_a_sign_in_the_restore_cannot_remove_is_named_as_left_in_the_nevr_root(self):
        result = self.run_script("--dll", str(self.dll), env=dict(self.env, FAKE_WINE_PIN_SIGNIN="1"))
        self.assertIn("the run's sign-in is still in the nevr root", result.stderr, result.stdout + result.stderr)
        self.assertNotIn("removed from the nevr root ===", result.stdout, "the restore claimed a clean copy")
        self.assertEqual(self.deployed(), ORIGINAL)

    def test_the_stored_sign_in_link_is_removed_when_the_run_is_interrupted(self):
        started = Fifo(self.tmp / "game-started")
        self.addCleanup(started.close)
        env = dict(self.env, FAKE_WINE_SLEEP="3", FAKE_WINE_STARTED=str(started.path))
        process = subprocess.Popen([str(self.checkout / "launch-client.sh"), "--dll", str(self.dll), "--creds",
                                    ACCOUNT], env=env, cwd=self.checkout, stdout=subprocess.PIPE,
                                   stderr=subprocess.STDOUT, text=True)
        try:
            started.wait_for(1)
            self.assertTrue(os.path.islink(self.link_path()), "the link was not in place while the game ran")
            process.send_signal(signal.SIGTERM)
            process.wait(timeout=60)
        finally:
            if process.poll() is None:
                process.kill()
            process.stdout.close()
        self.assertFalse(os.path.lexists(self.link_path()), "the link survived the interrupt")
        self.assertEqual(self.deployed(), ORIGINAL)

    # --- a refreshed sign-in is saved back to the store, and the exit trap never stops early (#1469) ---

    def store_names(self) -> list:
        return sorted(p.name for p in self.store.parent.iterdir())

    def bin_with_failing(self, name: str, condition: str) -> pathlib.Path:
        """A directory holding a `name` that fails while `condition` (a bash test on "$@") holds, else runs the real one."""
        directory = self.tmp / f"fail-{name}-{abs(hash(condition))}"
        directory.mkdir(exist_ok=True)
        real = shutil.which(name)
        (directory / name).write_text(
            "#!/bin/bash\n"
            f'if {condition}; then echo "{name}: injected failure" >&2; exit 1; fi\n'
            f'exec {real} "$@"\n')
        (directory / name).chmod(0o755)
        return directory

    def test_a_sign_in_the_runtime_refreshed_is_saved_back_to_the_store_at_0600(self):
        result = self.run_script("--dll", str(self.dll), "--creds", ACCOUNT,
                                 env=dict(self.env, FAKE_WINE_REPLACE_SIGNIN=ROTATED))
        out = result.stdout + result.stderr
        self.assertEqual(result.returncode, 0, out)
        self.assertEqual(self.store.read_bytes(), ROTATED.encode(), "the refreshed sign-in was not saved back")
        self.assertEqual(self.store.stat().st_mode & 0o777, 0o600)
        self.assertIn("refreshed sign-in saved back to the store", result.stdout)
        self.assertNotIn(ROTATED_SENTINEL, out)
        self.assertFalse(os.path.lexists(self.link_path()), "the run's copy was left in the nevr root")
        self.assertEqual(self.store_names(), [".credentials.json"], "a temporary file was left in the store")

    def test_a_sign_in_that_is_still_a_link_is_not_written_back(self):
        decoy = self.tmp / "decoy-signin"
        decoy.write_text(ROTATED)
        for label, extra in (("the link to the store", {}), ("a link repointed to another file",
                                                              {"FAKE_WINE_REPOINT": str(decoy)})):
            with self.subTest(case=label):
                self.store.write_bytes(SIGNIN)
                self.store.chmod(0o600)
                result = self.run_script("--dll", str(self.dll), "--creds", ACCOUNT, env=dict(self.env, **extra))
                out = result.stdout + result.stderr
                self.assertEqual(result.returncode, 0, out)
                self.assertEqual(self.store.read_bytes(), SIGNIN, "the store took bytes from the run's copy")
                self.assertNotIn("saved back", out)
                self.assertEqual(self.store_names(), [".credentials.json"])

    def test_a_regular_file_identical_to_the_store_is_not_rewritten(self):
        inode = self.store.stat().st_ino
        result = self.run_script("--dll", str(self.dll), "--creds", ACCOUNT,
                                 env=dict(self.env, FAKE_WINE_REPLACE_SIGNIN=SIGNIN.decode()))
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(self.store.stat().st_ino, inode, "an identical sign-in replaced the stored file")
        self.assertNotIn("saved back", result.stdout)

    def test_a_failing_restore_copy_still_runs_the_later_steps_and_the_run_exits_nonzero(self):
        bindir = self.bin_with_failing("cp", '[[ "${@: -2:1}" == *.orig ]]')
        wslog = self.tmp / "wineserver.log"
        env = dict(self.env, PATH=f"{bindir}:{self.env['PATH']}", FAKE_WS_LOG=str(wslog),
                   FAKE_WS_DLL=str(self.game_root / "echovr/bin/win10/BugSplat64.dll"))
        result = self.run_script("--dll", str(self.dll), "--creds", ACCOUNT, env=env)
        out = result.stdout + result.stderr
        self.assertNotEqual(result.returncode, 0, out)
        self.assertIn("could not restore", result.stderr, out)
        self.assertIn("ERROR: BugSplat64.dll restore failed", result.stderr, out)
        self.assertIn("-k dll=", wslog.read_text(), "the Wine server stop after the failed copy did not run")
        self.assertFalse(self.root_runtime.exists(), "the removal after the failed copy did not run")
        self.assertFalse(os.path.lexists(self.link_path()), "the sign-in removal after the failed copy did not run")

    def test_a_failing_removal_of_the_runtime_is_named_and_the_later_steps_still_run(self):
        bindir = self.bin_with_failing("rm", '[[ "$*" == *nevr-runtime.dll* ]]')
        wslog = self.tmp / "wineserver.log"
        env = dict(self.env, PATH=f"{bindir}:{self.env['PATH']}", FAKE_WS_LOG=str(wslog),
                   FAKE_WS_DLL=str(self.game_root / "echovr/bin/win10/BugSplat64.dll"))
        result = self.run_script("--dll", str(self.dll), "--creds", ACCOUNT, env=env)
        out = result.stdout + result.stderr
        self.assertNotEqual(result.returncode, 0, out)
        self.assertIn("could not remove", result.stderr, out)
        self.assertEqual(self.deployed(), ORIGINAL, "the DLL restore after the failed removal did not run")
        self.assertIn("-k dll=", wslog.read_text(), "the Wine server stop after the failed removal did not run")
        self.assertFalse(os.path.lexists(self.link_path()), "the sign-in removal after the failed removal did not run")

    def test_a_failing_copy_back_keeps_the_refreshed_sign_in_and_the_stored_one_and_exits_nonzero(self):
        bindir = self.bin_with_failing("cp", '[[ "${@: -1}" == *.saveback.* ]]')
        wslog = self.tmp / "wineserver.log"
        env = dict(self.env, PATH=f"{bindir}:{self.env['PATH']}", FAKE_WINE_REPLACE_SIGNIN=ROTATED,
                   FAKE_WS_LOG=str(wslog), FAKE_WS_DLL=str(self.game_root / "echovr/bin/win10/BugSplat64.dll"))
        result = self.run_script("--dll", str(self.dll), "--creds", ACCOUNT, env=env)
        out = result.stdout + result.stderr
        self.assertNotEqual(result.returncode, 0, out)
        self.assertIn("could not save the refreshed sign-in back", result.stderr, out)
        self.assertNotIn("restored and verified", result.stdout, "the verdict called a failed save-back clean")
        self.assertEqual(self.store.read_bytes(), SIGNIN, "the stored sign-in changed after a failed save")
        self.assertEqual(self.store_names(), [".credentials.json"], "a temporary file was left in the store")
        match = re.search(r"kept at (\S+?)[;\s]", result.stderr)
        self.assertIsNotNone(match, "the failed save does not name where the refreshed sign-in is kept")
        self.assertEqual(pathlib.Path(match.group(1)).read_bytes(), ROTATED.encode(),
                         "the refreshed sign-in was not kept")
        self.assertNotIn(ROTATED_SENTINEL, out)
        self.assertIn("-k dll=", wslog.read_text(), "the Wine server stop after the failed save did not run")
        self.assertFalse(self.root_runtime.exists(), "the removal after the failed save did not run")

    def test_a_device_code_run_starts_with_no_sign_in_and_says_where_the_code_is(self):
        result = self.run_script("--dll", str(self.dll), "--device-code", auth=())
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("DISPLAY :101", result.stdout)
        self.assertIn("screenshot", result.stdout)
        self.assertIn("masks", result.stdout)
        # The default login window of a device-code run: the code's 5 minutes plus margin (420 s), not the stock 120 s.
        self.assertIn("login window is 420 s", result.stdout)
        self.assertNotIn(SENTINEL, result.stdout + result.stderr)

    def test_a_device_code_run_keeps_an_explicit_login_timeout(self):
        result = self.run_script("--dll", str(self.dll), "--device-code", "--login-timeout", "50", auth=())
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("login window is 50 s", result.stdout)

    def test_a_device_code_run_ends_itself_at_login(self):
        env = dict(self.env, FAKE_WINE_SLEEP="30", NEVR_LOGIN_POLL_SECONDS="0.1")
        started = time.monotonic()
        result = self.run_script("--dll", str(self.dll), "--device-code", env=env, auth=())
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertLess(time.monotonic() - started, 5.0, "the run waited for the game to exit instead of ending at login")

    def test_a_device_code_run_ends_at_the_runtimes_failure_lines_and_restores(self):
        lines = ("[NEVR.AUTH] Device auth timed out after 5 minutes",
                 "[NEVR.AUTH] Device code expired. Please restart to try again.",
                 "[NEVR.AUTH] device authorization stopped because the browser could not be opened")
        for line in lines:
            with self.subTest(line=line):
                env = dict(self.env, FAKE_LOG_TEXT=json.dumps({"msg": line}) + "\\n", FAKE_WINE_SLEEP="30",
                           NEVR_LOGIN_POLL_SECONDS="0.1")
                started = time.monotonic()
                result = self.run_script("--dll", str(self.dll), "--device-code", env=env, auth=())
                self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
                self.assertLess(time.monotonic() - started, 8.0,
                                "the run waited out the game instead of ending at the failure line")
                self.assertIn("device-code flow ended without a sign-in", result.stderr)
                self.assertIn("original BugSplat64.dll restored and verified", result.stdout)
                self.assertEqual(self.deployed(), ORIGINAL)

    def test_a_device_code_run_is_not_ended_by_lines_that_are_not_failures(self):
        text = (json.dumps({"msg": "[NEVR.AUTH] Device auth cancelled: the game is closing"}) + "\\n"
                + json.dumps({"msg": "[NEVR.AUTH] Device code expired before a sign-in; a new code will be requested"})
                + "\\n" + '{"msg":"NetGame switching state (from logging in, to logged in)"}\\n')
        result = self.run_script("--dll", str(self.dll), "--device-code", env=dict(self.env, FAKE_LOG_TEXT=text),
                                 auth=())
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_a_device_code_run_saves_the_new_sign_in_at_0600_and_never_overwrites(self):
        store = self.work / "creds" / "newacct" / ".credentials.json"
        env = dict(self.env, FAKE_WINE_WRITE_SIGNIN=SIGNIN.decode())
        result = self.run_script("--dll", str(self.dll), "--device-code", "--save-creds", "newacct", env=env, auth=())
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(store.read_bytes(), SIGNIN)
        self.assertEqual(store.stat().st_mode & 0o777, 0o600)
        self.assertFalse(os.path.lexists(self.link_path()), "the run's sign-in was left in the copy")
        self.assertNotIn(SENTINEL, result.stdout + result.stderr)
        again = self.run_script("--dll", str(self.dll), "--device-code", "--save-creds", "newacct", env=env, auth=())
        self.assertEqual(again.returncode, 3, again.stdout + again.stderr)
        self.assertIn("already stored", again.stderr)
        self.assertEqual(store.read_bytes(), SIGNIN)

    def test_save_creds_makes_the_account_and_creds_directories_mode_700(self):
        # A work root with no creds/ yet, and the process umask at 022 (the usual one: a plain mkdir would make 755).
        fresh = self.tmp / "fresh-work"
        env = dict(self.env, NEVR_WORK_ROOT=str(fresh), FAKE_WINE_WRITE_SIGNIN=SIGNIN.decode())
        old_umask = os.umask(0o022)
        self.addCleanup(os.umask, old_umask)
        result = self.run_script("--dll", str(self.dll), "--device-code", "--save-creds", "newacct", env=env, auth=())
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        account_dir = fresh / "creds" / "newacct"
        self.assertEqual(stat.S_IMODE(account_dir.stat().st_mode), 0o700, "the account directory is not mode 700")
        self.assertEqual(stat.S_IMODE((fresh / "creds").stat().st_mode), 0o700, "creds/ is not mode 700")
        self.assertEqual(stat.S_IMODE((account_dir / ".credentials.json").stat().st_mode), 0o600)
        self.assertNotIn(SENTINEL, result.stdout + result.stderr)

    def test_creds_without_an_account_is_a_usage_error(self):
        result = self.run_script("--dll", str(self.dll), "--creds", auth=())
        self.assertEqual(result.returncode, 2, result.stdout + result.stderr)
        self.assertIn("--creds needs an account name", result.stderr)
        self.assertEqual(self.deployed(), ORIGINAL)
        self.assertFalse(self.lock.exists(), "the run lock was taken before the refusal")

    def test_the_save_helper_never_replaces_a_stored_sign_in(self):
        # The helper's own check (a hard link fails on a path that exists), below the launcher's pre-lock check.
        source = self.tmp / "new-signin.json"
        source.write_bytes(SIGNIN)
        path = self.work / "creds" / "helper-account" / ".credentials.json"
        cmd = f"set -eu; source {LIB}; save_stored_sign_in {source} {path}"
        first = subprocess.run(["bash", "-c", cmd], env=self.env, capture_output=True, text=True, timeout=30)
        self.assertEqual(first.returncode, 0, first.stdout + first.stderr)
        self.assertEqual(path.read_bytes(), SIGNIN)
        self.assertEqual(path.stat().st_mode & 0o777, 0o600)
        source.write_bytes(b'{"refresh_token":"OTHER-SENTINEL"}')
        second = subprocess.run(["bash", "-c", cmd], env=self.env, capture_output=True, text=True, timeout=30)
        self.assertEqual(second.returncode, 1, second.stdout + second.stderr)
        self.assertIn("already exists", second.stderr)
        self.assertEqual(path.read_bytes(), SIGNIN, "a stored sign-in was replaced")
        self.assertEqual(list(path.parent.glob(".credentials.json.new.*")), [], "a temporary copy was left behind")

    def test_save_creds_needs_device_code_and_no_stored_sign_in_to_use(self):
        for args in (("--save-creds", "x"), ("--creds", ACCOUNT, "--device-code", "--save-creds", "x")):
            with self.subTest(args=args):
                result = self.run_script("--dll", str(self.dll), *args, auth=())
                self.assertEqual(result.returncode, 2, result.stdout + result.stderr)
        self.assertEqual(self.deployed(), ORIGINAL)

    def test_timeout_must_be_a_positive_whole_number(self):
        for bad in ("0", "-5", "1.5", "soon"):
            with self.subTest(value=bad):
                result = self.run_script("--dll", str(self.dll), "--timeout", bad)
                self.assertEqual(result.returncode, 2, result.stdout + result.stderr)
                self.assertIn("--timeout must be", result.stderr)
        self.assertEqual(self.deployed(), ORIGINAL)

    def test_a_client_that_never_logs_in_is_ended_at_timeout_and_restored(self):
        # The dead proxy: the game logs no login and hangs. --timeout ends it well inside the 15 s kill-after.
        env = dict(self.env, FAKE_LOG_TEXT='{"msg":"noise"}\\n', FAKE_WINE_SLEEP="600")
        started = time.monotonic()
        result = self.run_script("--dll", str(self.dll), "--timeout", "3", env=env)
        elapsed = time.monotonic() - started
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertLess(elapsed, 12.0, "the client outlived --timeout")
        self.assertIn("--timeout 3 s elapsed", result.stderr)
        self.assertIn("original BugSplat64.dll restored and verified", result.stdout)
        self.assertEqual(self.deployed(), ORIGINAL)
        self.assertFalse(os.path.lexists(self.link_path()))

    def test_a_device_code_client_that_never_logs_in_is_ended_at_timeout_and_restored(self):
        # The exit-after-login branch (a device-code run ends at login) is bounded by --timeout too.
        env = dict(self.env, FAKE_LOG_TEXT='{"msg":"noise"}\\n', FAKE_WINE_SLEEP="600")
        started = time.monotonic()
        result = self.run_script("--dll", str(self.dll), "--device-code", "--timeout", "3", env=env, auth=())
        elapsed = time.monotonic() - started
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertLess(elapsed, 12.0, "the client outlived --timeout")
        self.assertIn("--timeout 3 s elapsed", result.stderr)
        self.assertIn("original BugSplat64.dll restored and verified", result.stdout)
        self.assertEqual(self.deployed(), ORIGINAL)
        self.assertFalse(os.path.lexists(self.link_path()))

    def test_a_client_that_ignores_the_interrupt_is_killed_at_timeout_and_restored(self):
        # timeout sends INT, the client ignores it, and timeout's KILL (-k 15) ends it: the status is 137, not 124, and
        # the run still says the timeout elapsed (a 137 counts only when the run lasted the whole --timeout).
        env = dict(self.env, FAKE_LOG_TEXT='{"msg":"noise"}\\n', FAKE_WINE_SLEEP="600", FAKE_WINE_IGNORE_INT="1")
        started = time.monotonic()
        result = self.run_script("--dll", str(self.dll), "--timeout", "2", env=env)
        elapsed = time.monotonic() - started
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertGreaterEqual(elapsed, 2.0)
        self.assertLess(elapsed, 25.0, "the client outlived --timeout and its kill-after")
        self.assertIn("--timeout 2 s elapsed", result.stderr)
        self.assertIn("original BugSplat64.dll restored and verified", result.stdout)
        self.assertEqual(self.deployed(), ORIGINAL)

    def test_the_game_is_started_with_the_host_browser_blocked(self):
        seen = self.tmp / "xdg-seen"
        result = self.run_script("--dll", str(self.dll), env=dict(self.env, FAKE_WINE_XDG=str(seen)))
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(os.path.dirname(seen.read_text().strip()), str(self.work / "browser-stub"),
                         "the game reaches a host xdg-open first")


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
            NEVR_WORK_ROOT=str(self.tmp / "work"),
            NEVR_VERIFY_MIN_SECONDS="1",
            NEVR_VERIFY_POLL_SECONDS="1",
            FAKE_EXPECT_DLL=str(bin_dir / "BugSplat64.dll"),
            FAKE_EXPECT_RUNTIME=str(bin_dir / "nevr-runtime.dll"),
        )
        self.env.pop("FAKE_ECHOVR_PIDS", None)

    def command(self):
        return [str(self.checkout / "verify-server.sh"), "run1", "default", "1"]

    def test_the_summary_names_a_host_stop_and_says_none_otherwise(self):
        # #1122: a host that refuses nevr-runtime.dll leaves no game log; the summary says why from the host's record.
        # The host ends the game with exit 78; the game exits at once, so the run fails and says why (#1308).
        stopped = subprocess.run(self.command(), env=dict(self.env, FAKE_HOST_STOP="unsigned", FAKE_WINE_EXIT="78"),
                                 cwd=self.checkout, capture_output=True, text=True, timeout=60)
        self.assertEqual(stopped.returncode, 1, stopped.stdout + stopped.stderr)
        self.assertIn("verify-server: FAIL run1: the game exited with code 78 before the run window ended",
                      stopped.stderr)
        self.assertIn("host stop           = the host stopped the game (exit 78, reason runtime_refused): "
                      "the host refused nevr-runtime.dll: unsigned", stopped.stdout)
        # The fake runs are under a second apart; a real boot is not, so the first run's record is replaced by one
        # from an earlier day: a stop the host wrote before this run is not this run's.
        (self.game_root / "echovr/.wineprefix/drive_c/users" / getpass.getuser()
         / "AppData/Local/nevr/echovr/logs/nevr-bootstrap.jsonl").write_text(
            '{"event":"game_stop","exit_code":78,"reason":"runtime_refused","run":"old","ts":"2026-10-01T00:00:00.000Z"}\n')
        normal = subprocess.run(self.command(), env=self.env, cwd=self.checkout, capture_output=True, text=True,
                                timeout=60)
        self.assertEqual(normal.returncode, 0, normal.stdout + normal.stderr)
        self.assertIn("host stop           = none", normal.stdout)

    def test_a_game_that_exits_nonzero_fails_the_run(self):
        # #1308: the exit status is the run's verdict, not the summary pipeline's.
        result = self.run_verify(dict(self.env, FAKE_WINE_EXIT="3"))
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn("the game exited with code 3 before the run window ended", result.stderr)
        self.assertIn("result              = FAIL: the game exited with code 3", result.stdout)

    def test_a_run_whose_log_lacks_the_boot_line_fails(self):
        result = self.run_verify(dict(self.env, FAKE_WINE_NO_BOOT="1"))
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn("no 'Finished initializing engine' line", result.stderr)

    def test_a_healthy_run_passes_and_names_its_config_and_capture(self):
        result = self.run_verify()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("result              = PASS", result.stdout)
        self.assertIn("config              = offline", result.stdout)
        self.assertIn("counts read from    = ", result.stdout)

    def test_an_install_config_run_must_log_in(self):
        # With the install's own config.yaml the run is a login run: a log without LOGIN SUCCESS fails (#1308).
        (self.game_root / "echovr/_local/config.yaml").write_text("services: {}\n")
        result = self.run_verify(dict(self.env, FAKE_WINE_NO_LOGIN="1"))
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn("no 'LOGIN SUCCESS' line", result.stderr)
        passed = self.run_verify()
        self.assertEqual(passed.returncode, 0, passed.stdout + passed.stderr)
        self.assertIn("config              = install", passed.stdout)

    def test_verify_server_refuses_to_run_without_a_copy_named(self):
        # #1308: the install itself is never a verification target; NEVR_GAME_ROOT must name a copy.
        env = {k: v for k, v in self.env.items() if k != "NEVR_GAME_ROOT"}
        result = self.run_verify(env)
        self.assertEqual(result.returncode, 2, result.stdout + result.stderr)
        self.assertIn("needs NEVR_GAME_ROOT", result.stderr)
        self.assertFalse((self.tmp / "server-runs/run1").exists(), "a refused run must not write its output")

    def test_a_finished_run_leaves_the_game_directory_as_it_found_it(self):
        result = subprocess.run(self.command(), env=self.env, cwd=self.checkout, capture_output=True,
                                text=True, timeout=60)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual((self.win10 / "BugSplat64.dll").read_bytes(), ORIGINAL)
        self.assertFalse((self.win10 / "plugins/extra.dll").exists(), "a deployed plugin was left behind")
        self.assertFalse((self.win10 / "nevr-runtime.dll").exists(), "the deployed runtime was left behind")
        self.assertFalse((nevr_root_of(self.game_root) / "nevr-runtime.dll").exists(),
                         "the deployed runtime was left behind in the nevr root")
        self.assertIn("deployed files restored and verified", (self.tmp / "server-runs/run1/server.log").read_text())

    def test_the_game_log_is_copied_out_of_the_nevr_root_into_the_run_directory(self):
        # #1297: the run's log is read from the nevr root's logs folder, and a copy is kept with the run.
        result = subprocess.run(self.command(), env=self.env, cwd=self.checkout, capture_output=True,
                                text=True, timeout=60)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        copy = self.tmp / "server-runs/run1/game-log.jsonl"
        self.assertIn("to logged in", copy.read_text())
        self.assertIn(f"game log copy       = {copy}", result.stdout)

    def test_a_game_log_that_cannot_be_copied_out_fails_the_run(self):
        # A cp that fails for the game log only: the run must fail loudly, not pass without its log.
        bindir = self.tmp / "fail-cp"
        bindir.mkdir()
        (bindir / "cp").write_text(
            "#!/bin/bash\n"
            'if [[ "$*" == *game-log.jsonl* ]]; then echo "cp: injected failure" >&2; exit 1; fi\n'
            f'exec {shutil.which("cp")} "$@"\n')
        (bindir / "cp").chmod(0o755)
        env = dict(self.env, PATH=f"{bindir}:{self.env['PATH']}")
        result = subprocess.run(self.command(), env=env, cwd=self.checkout, capture_output=True,
                                text=True, timeout=60)
        self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("could not copy the game log", result.stderr)

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

    def test_the_game_is_started_with_the_host_browser_blocked(self):
        seen = self.tmp / "xdg-seen"
        result = self.run_verify(dict(self.env, FAKE_WINE_XDG=str(seen)))
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(os.path.dirname(seen.read_text().strip()), str(self.tmp / "work" / "browser-stub"),
                         "the game reaches a host xdg-open first")

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
        pid = game_standin(self)
        env = dict(self.env, FAKE_ECHOVR_PIDS=str(pid))
        result = subprocess.run(self.command(), env=env, cwd=self.checkout, capture_output=True, text=True,
                                timeout=60)
        self.assertEqual(result.returncode, 4, result.stdout + result.stderr)
        self.assertIn(f"echovr.exe is already running (pid {pid})", result.stderr)
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
            NEVR_WORK_ROOT=str(self.tmp / "work"),
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
        self.assertEqual((nevr_root_of(self.game_root) / "nevr-runtime.dll").read_bytes(), TEST_RUNTIME)
        self.assertFalse((self.win10 / "nevr-runtime.dll").exists(), "the host never loads a runtime beside itself")
        self.assertFalse((self.checkout / "echovr").exists())

    def test_refuses_while_another_game_run_holds_the_lock(self):
        with open(self.tmp / "launch.lock", "w") as held:
            fcntl.flock(held, fcntl.LOCK_EX | fcntl.LOCK_NB)
            result = self.run_script()
        self.assertEqual(result.returncode, 4, result.stdout + result.stderr)
        self.assertEqual((self.win10 / "BugSplat64.dll").read_bytes(), ORIGINAL)

    def test_refuses_while_echovr_is_running(self):
        pid = game_standin(self)
        result = self.run_script(dict(self.env, FAKE_ECHOVR_PIDS=str(pid)))
        self.assertEqual(result.returncode, 4, result.stdout + result.stderr)
        self.assertIn(f"echovr.exe is already running (pid {pid})", result.stderr)
        self.assertEqual((self.win10 / "BugSplat64.dll").read_bytes(), ORIGINAL)

    def test_a_missing_game_install_is_a_clear_error(self):
        result = self.run_script(dict(self.env, NEVR_GAME_ROOT=str(self.tmp / "nowhere")))
        self.assertEqual(result.returncode, 2, result.stdout + result.stderr)
        self.assertIn("no game install", result.stderr)

    def test_the_game_is_started_with_the_host_browser_blocked(self):
        seen = self.tmp / "xdg-seen"
        result = self.run_script(dict(self.env, FAKE_WINE_XDG=str(seen)))
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(os.path.dirname(seen.read_text().strip()), str(self.tmp / "work" / "browser-stub"),
                         "the game reaches a host xdg-open first")


@unittest.skipUnless(REAL_WINE and REAL_WINESERVER, "wine is not installed")
class HostBrowserChainTest(unittest.TestCase):
    """The real Wine, once, for the URL chain a game's ShellExecute takes (#1447). The launchers put a stub `xdg-open`
    first on PATH (tools/lib/game_install.sh block_host_browser); the first test shows that a URL open under that
    environment reaches the stub. The control shows that without it the same open reaches the first `xdg-open` on
    PATH, with the full URL, so the first test would have caught a host browser. The last test shows that Wine follows
    the symlink a stored sign-in is linked in by. Nothing here starts the game; the browser is a recorder, never a
    real one, and DISPLAY and WAYLAND_DISPLAY are unset for every Wine call."""

    URL = "https://example.invalid/device?code=TESTCODE"

    @classmethod
    def setUpClass(cls):
        cls.tmp = pathlib.Path(tempfile.mkdtemp(prefix="browser-chain-test-", dir="/var/tmp"))
        cls.prefix = cls.tmp / "prefix"
        cls.env = {k: v for k, v in os.environ.items()
                   if k not in ("DISPLAY", "WAYLAND_DISPLAY", "XDG_SESSION_TYPE", "HYPRLAND_INSTANCE_SIGNATURE")}
        cls.env.update(WINEPREFIX=str(cls.prefix), WINEDEBUG="-all")
        boot = subprocess.run([REAL_WINE, "wineboot", "--init"], env=cls.env, capture_output=True, text=True,
                              timeout=900)
        subprocess.run([REAL_WINESERVER, "-w"], env=cls.env, timeout=300)
        if boot.returncode != 0:
            raise RuntimeError(f"wineboot --init failed: {boot.stdout}{boot.stderr}")

    @classmethod
    def tearDownClass(cls):
        subprocess.run([REAL_WINESERVER, "-k"], env=cls.env, capture_output=True, timeout=60)  # 1 when none is left
        shutil.rmtree(cls.tmp)

    def open_url(self, setup: str, work: pathlib.Path) -> None:
        """Run `setup` (bash) and then `wine start <URL>` with the chain's environment, and wait until Wine is idle."""
        env = dict(self.env, NEVR_WORK_ROOT=str(work))
        done = subprocess.run(["bash", "-c", f'set -eu; {setup}; wine start "$1"', "bash", self.URL], env=env,
                              capture_output=True, text=True, timeout=300)
        self.assertEqual(done.returncode, 0, done.stdout + done.stderr)
        subprocess.run([REAL_WINESERVER, "-w"], env=self.env, timeout=120)  # the open runs in Wine's own processes

    @staticmethod
    def wait_for_file(path: pathlib.Path, seconds: float = 30.0):
        deadline = time.monotonic() + seconds
        while not path.exists() and time.monotonic() < deadline:
            time.sleep(0.5)
        return path.read_text() if path.exists() else None

    def test_a_url_open_under_the_launcher_environment_is_taken_by_the_stub(self):
        work = self.tmp / "work-stub"
        self.open_url(f"source {LIB}; block_host_browser", work)
        text = self.wait_for_file(work / "browser-stub" / "blocked.log")
        self.assertIsNotNone(text, "the open did not reach the stub")
        self.assertIn("blocked https://example.invalid/device", text)
        self.assertNotIn("TESTCODE", text, "the device code reached the stub's log")

    def test_without_the_stub_the_same_open_reaches_the_first_xdg_open_on_path(self):
        rec = self.tmp / "rec-bin"
        rec.mkdir()
        record = self.tmp / "control.record"
        (rec / "xdg-open").write_text(f'#!/bin/bash\nprintf "%s\\n" "$1" > "{record}"\nexit 0\n')
        (rec / "xdg-open").chmod(0o755)
        self.open_url(f'export PATH="{rec}:$PATH"', self.tmp / "work-control")
        text = self.wait_for_file(record)
        self.assertEqual(text.strip() if text else None, self.URL, "the control did not see the open")

    def test_wine_reads_a_sign_in_through_a_symlink_in_the_nevr_root(self):
        store = self.tmp / "store" / ".credentials.json"
        store.parent.mkdir(parents=True, exist_ok=True)
        store.write_text(LINK_MARKER)
        # The runtime makes the nevr root on its first run; the prefix from wineboot does not have it yet.
        root = self.prefix / "drive_c/users" / getpass.getuser() / "AppData/Local/nevr/echovr"
        root.mkdir(parents=True, exist_ok=True)
        link = root / ".credentials.json"
        link.symlink_to(store)
        self.addCleanup(os.remove, link)
        done = subprocess.run([REAL_WINE, "cmd", "/c", "type", "%LOCALAPPDATA%\\nevr\\echovr\\.credentials.json"],
                              env=self.env, capture_output=True, text=True, timeout=300)
        self.assertIn(LINK_MARKER, done.stdout, done.stdout + done.stderr)


if __name__ == "__main__":
    unittest.main()
