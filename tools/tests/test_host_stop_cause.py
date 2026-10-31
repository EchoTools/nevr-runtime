"""#1122: a run whose host stops the game names the cause from the host's own record.

The lines are the ones the host wrote on the night the scenario smoke failed (runtime_load, then game_stop), with the
volatile fields kept as the host writes them. The behaviour of the host itself is pinned by test_host_runtime_verify.
"""
import datetime
import json
import pathlib
import subprocess
import sys
import tempfile
import time
import unittest

TOOLS = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(TOOLS))
sys.path.insert(0, str(TOOLS / "scenario"))
import host_stop_cause  # noqa: E402
import run_scenario  # noqa: E402

RUNTIME = "Z:\\work\\nevr-runtime\\echovr\\bin\\win10\\nevr-runtime.dll"


def iso(epoch: float) -> str:
    return datetime.datetime.fromtimestamp(epoch, datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%S.%f")[:-3] + "Z"


def stop_records(at: float, reason: str = "unsigned", outcome: str = "runtime_refused", run: str = "36eba0205775b5e9"):
    load = {"degraded_action": "stop_game", "event": "runtime_load", "expected_abi": "1.1", "outcome": outcome,
            "path": RUNTIME, "path_ok": True, "pid": 32, "run": run, "tag": "NEVR.HOST", "ts": iso(at)}
    if outcome == "runtime_refused":
        load.update({"verify": "refused", "verify_reason": reason})
    stop = {"event": "game_stop", "exit_code": 78, "host_identity": "NEVR-BUILD 5.16.1-dev.1+c2eabdc2 c2eabdc2",
            "notice": "shown", "pid": 32, "reason": outcome, "run": run, "runtime_path": RUNTIME, "tag": "NEVR.HOST",
            "ts": iso(at + 0.003)}
    return [load, stop]


def loaded_records(at: float, run: str = "95eac89fb2d28f0f"):
    return [{"event": "runtime_load", "outcome": "loaded", "path": RUNTIME, "run": run, "tag": "NEVR.HOST", "ts": iso(at)}]


class HostStopCauseTest(unittest.TestCase):
    def setUp(self):
        self.tmp = pathlib.Path(tempfile.mkdtemp(prefix="host-stop-cause-test-", dir="/var/tmp"))
        self.addCleanup(lambda: __import__("shutil").rmtree(self.tmp))
        self.log = self.tmp / "nevr-bootstrap.jsonl"
        self.now = time.time()

    def write(self, *groups, raw=""):
        lines = [json.dumps(r) for group in groups for r in group]
        self.log.write_text("\n".join(lines) + "\n" + raw, encoding="utf-8")

    def test_an_unsigned_runtime_is_named_with_the_build_that_fixes_it(self):
        self.write(stop_records(self.now))
        text = host_stop_cause.cause(self.log, self.now - 5)
        self.assertIn("the host refused nevr-runtime.dll: unsigned", text)
        self.assertIn("exit 78", text)
        self.assertIn("build with mingw-plugin-dev or mingw-scenario", text)
        self.assertIn(RUNTIME, text)

    def test_a_changed_runtime_is_named_without_advice_to_build_a_dev_build(self):
        self.write(stop_records(self.now, reason="the file was changed after it was signed"))
        text = host_stop_cause.cause(self.log, self.now - 5)
        self.assertIn("the host refused nevr-runtime.dll: the file was changed after it was signed", text)
        self.assertNotIn("mingw-plugin-dev", text)

    def test_a_missing_runtime_is_named(self):
        self.write(stop_records(self.now, outcome="runtime_absent"))
        text = host_stop_cause.cause(self.log, self.now - 5)
        self.assertIn("nevr-runtime.dll is not beside BugSplat64.dll", text)

    def test_a_stop_from_before_the_run_is_not_this_runs(self):
        self.write(stop_records(self.now - 100))
        self.assertIsNone(host_stop_cause.cause(self.log, self.now - 5))

    def test_a_run_that_loaded_its_runtime_has_no_stop(self):
        self.write(loaded_records(self.now))
        self.assertIsNone(host_stop_cause.cause(self.log, self.now - 5))

    def test_the_newest_stop_wins(self):
        self.write(stop_records(self.now - 3, reason="unsigned", run="a"),
                   stop_records(self.now, reason="the file was changed after it was signed", run="b"))
        self.assertIn("changed after it was signed", host_stop_cause.cause(self.log, self.now - 10))

    def test_another_runs_refusal_is_not_this_stops_reason(self):
        # Two processes share a prefix (a client and a local server): the reason is the one the stop's own run wrote.
        self.write(stop_records(self.now, reason="unsigned", run="a"),
                   [{"event": "runtime_load", "outcome": "runtime_refused", "verify_reason": "digest mismatch",
                     "path": RUNTIME, "run": "b", "ts": iso(self.now)}])
        text = host_stop_cause.cause(self.log, self.now - 5)
        self.assertIn("refused nevr-runtime.dll: unsigned", text)
        self.assertNotIn("digest mismatch", text)

    def test_a_torn_line_does_not_hide_the_records_around_it(self):
        self.write(stop_records(self.now), raw='{"event":"game_st')
        self.assertIn("the host refused nevr-runtime.dll: unsigned", host_stop_cause.cause(self.log, self.now - 5))

    def test_an_absent_record_says_nothing(self):
        self.assertIsNone(host_stop_cause.cause(self.tmp / "none.jsonl", 0))

    def run_cli(self, since):
        return subprocess.run([sys.executable, "-I", str(TOOLS / "host_stop_cause.py"), "--log", str(self.log),
                               "--since", str(since)], capture_output=True, text=True)

    def test_the_command_prints_the_cause_and_exits_0(self):
        self.write(stop_records(self.now))
        result = self.run_cli(self.now - 5)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("the host refused nevr-runtime.dll: unsigned", result.stdout)

    def test_the_command_prints_nothing_and_exits_3_when_there_is_no_stop(self):
        self.write(loaded_records(self.now))
        result = self.run_cli(self.now - 5)
        self.assertEqual((result.returncode, result.stdout), (host_stop_cause.NO_STOP, ""))


class ScenarioWaitEndsOnAHostStop(unittest.TestCase):
    """The game stays up on the stop dialog nobody dismisses, so only the host's record ends the wait."""

    def setUp(self):
        self.tmp = pathlib.Path(tempfile.mkdtemp(prefix="host-stop-wait-test-", dir="/var/tmp"))
        self.addCleanup(lambda: __import__("shutil").rmtree(self.tmp))
        self.console = self.tmp / "console.log"
        self.console.write_text("MESA-EGL: warning: DRI3 error: Could not get DRI3 device\n")

    def test_a_wait_stops_at_once_with_the_hosts_words_while_the_game_is_up(self):
        log = run_scenario.ConsoleLog(self.console, halted=lambda: "the host stopped the game: x")
        started = time.monotonic()
        with self.assertRaisesRegex(run_scenario.StepFailed, "the host stopped the game: x; before /to in game/"):
            log.wait(r"to in game", 600, 0, alive=lambda: True)
        self.assertLess(time.monotonic() - started, 5)

    def test_the_runner_reads_the_stop_from_the_prefixs_nevr_root_since_the_launch(self):
        logs = self.tmp / "logs"
        logs.mkdir()
        run = run_scenario.Run.__new__(run_scenario.Run)
        run.launched_at = time.time() - 5
        original = run_scenario.nevr_root_logs
        run_scenario.nevr_root_logs = lambda: logs
        self.addCleanup(setattr, run_scenario, "nevr_root_logs", original)
        self.assertIsNone(run.host_stopped(), "no record yet")
        (logs / "nevr-bootstrap.jsonl").write_text("\n".join(json.dumps(r) for r in stop_records(time.time())) + "\n")
        self.assertIn("the host refused nevr-runtime.dll: unsigned", run.host_stopped())
        run.launched_at = time.time() + 60
        self.assertIsNone(run.host_stopped(), "a stop from before the launch is not this launch's")

    def test_the_prefixs_nevr_root_logs_folder_is_where_the_host_writes(self):
        import getpass
        original = run_scenario.wineprefix
        run_scenario.wineprefix = lambda: pathlib.Path("/prefix")
        self.addCleanup(setattr, run_scenario, "wineprefix", original)
        self.assertEqual(pathlib.Path("/prefix/drive_c/users") / getpass.getuser() / "AppData/Local/nevr/echovr/logs",
                         run_scenario.nevr_root_logs())

    def test_a_wait_without_a_stop_still_times_out(self):
        log = run_scenario.ConsoleLog(self.console, halted=lambda: None)
        self.assertIsNone(log.wait(r"to in game", 1, 0, alive=lambda: True))


if __name__ == "__main__":
    unittest.main()
