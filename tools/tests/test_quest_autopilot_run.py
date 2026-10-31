"""tools/quest_autopilot_run.py against a fake adb: no headset is touched (#1202).

The runner runs for real with a fake adb on the PATH that answers the commands it issues and logs every call (the
leading `-s <serial>` removed). The logcat stream is a file the fake prints in one burst, then holds open, as a live logcat does.
What a real headset prints for these commands is not verified here: that is the headset run owed on the pull request
(ADR 0025, predictions P1-P5).
"""
from __future__ import annotations

import json
import os
import datetime
import pathlib
import signal
import subprocess
import sys
import time
import unittest

REPO = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "tools"))
import quest_autopilot_run as run  # noqa: E402
import work_root  # noqa: E402

RUNNER = REPO / "tools/quest_autopilot_run.py"
SERIAL = "1WMHH8XQ4R0123"
FILES = "/sdcard/Android/data/com.readyatdawn.r15/files"
MEDIA_FILES = "/sdcard/Android/media/com.readyatdawn.r15/files"
ROUTE = f"{FILES}/autopilot/route.txt"
START = f"{FILES}/autopilot/start"

CONFIG = f"""services:
  socket_uri: wss://example.invalid
plugins:
  - name: other_plugin
    args:
      route: /wrong/path
  - name: nevr_autopilot
    enabled: true
    args:
      route: {ROUTE}
      start_file: "{START}"   # optional
"""

FAKE_ADB = r"""#!/usr/bin/env bash
if [[ "$1" == devices ]]; then printf 'List of devices attached\n%b' "${FAKE_DEVICES-""" + SERIAL + r"""\tdevice\n}"; exit 0; fi
[[ "$1" == -s ]] && shift 2
printf '%s\n' "$*" >> "$FAKE_ADB_LOG"
# FAKE_FAIL / FAKE_SLOW: a call that starts with this text fails (exit 1) / hangs.
[[ -n "${FAKE_FAIL:-}" && "$*" == "$FAKE_FAIL"* ]] && { echo "error: injected failure" >&2; exit 1; }
[[ -n "${FAKE_SLOW:-}" && "$*" == "$FAKE_SLOW"* ]] && exec sleep 600
case "$*" in
  "shell cat /sdcard/Android/data/com.readyatdawn.r15/files/config.yaml") cat "$FAKE_CONFIG" ;;
  "push "*) [[ -f "$2" ]] || exit 1; cp "$2" "$FAKE_PUSHED"; echo "1 file pushed" ;;
  "shell rm -f "*) ;;
  "shell touch "*) ;;
  "shell am broadcast -a "*) echo "Broadcast completed: result=0" ;;
  "shell am force-stop com.readyatdawn.r15") ;;
  "logcat -c") ;;
  "shell am start -n com.readyatdawn.r15/com.oculus.gles3jni.MainActivity") echo "Starting: Intent { cmp=com.readyatdawn.r15/com.oculus.gles3jni.MainActivity }" ;;
  "logcat -v epoch") echo $$ > "$FAKE_LOGCAT_PID"; cat "$FAKE_STREAM"; exec sleep "${FAKE_HOLD:-600}" ;;
  "pull "*) printf '{"event":"sentinel_ctor"}\n' > "$3"; echo "1 file pulled" ;;
  *) echo "unexpected adb call: $*" >&2; exit 9 ;;
esac
"""


def plugin(message: str, fields: str = "") -> str:
    return f"  1793212224.589  100  200 I NEVR-Sentinel: [NEVR.PLUGIN] plugin=nevr_autopilot {message} {fields}\n"


def host(record: dict) -> str:
    return f"  1793212224.500  100  200 I NEVR-Sentinel: {json.dumps(record)}\n"


LOADED = plugin("route_loaded", f"route={ROUTE} name=lobby_loop sha256={'a' * 64} steps=2 repeat=1 planned_s=2.000000000 "
                "start_after_s=0.000000000 start=first_frame drives=left+right")
REGISTERED = plugin("input_filter", "result=registered")
SEAM = host({"ts_ms": 1, "level": "info", "event": "input_seam", "result": "installed", "reason": "ok",
             "plugin": "nevr_autopilot", "slot": "vrapi_GetCurrentInputState", "status": "ok"})
LOAD = host({"ts_ms": 1, "level": "info", "event": "plugin_load", "name": "nevr_autopilot", "outcome": "loaded"})
START_LINE = plugin("route_start", "name=lobby_loop realtime_ms=1793212224600 monotonic_ns=5000 boottime_ns=6000")
STEPS = plugin("route_step", "index=0 pass=0 realtime_ms=1 monotonic_ns=1") + plugin(
    "route_step", "index=1 pass=0 realtime_ms=2 monotonic_ns=2")


def end_line(left: int = 120, right: int = 118) -> str:
    return plugin("route_end", f"name=lobby_loop realtime_ms=1793212226600 monotonic_ns=2000005000 boottime_ns=2000006000 "
                  f"planned_s=2.000000000 measured_s=2.011000000 frames_left={left} frames_right={right} "
                  f"frames_unknown_hand=0 frames_driven_left={left} frames_driven_right={right} "
                  f"max_frame_gap_ms=13.900000 events_dropped=0")


class RunnerTest(unittest.TestCase):
    def setUp(self):
        self._dir = work_root.make_test_dir("quest-autopilot-run-")
        self.addCleanup(self._dir.cleanup)
        self.tmp = pathlib.Path(self._dir.name)
        self.bin = self.tmp / "bin"
        self.bin.mkdir()
        (self.bin / "adb").write_text(FAKE_ADB)
        (self.bin / "adb").chmod(0o755)
        self.adb_log = self.tmp / "adb.log"
        self.config = self.tmp / "config.yaml"
        self.config.write_text(CONFIG)
        self.stream = self.tmp / "logcat.stream"
        self.pushed = self.tmp / "pushed.txt"
        self.route = self.tmp / "route.txt"
        self.route.write_text("autopilot-route 1\nname lobby_loop\nstep 1 left.stick=0,1\nstep 1 right.trigger=1\n")
        self.out = self.tmp / "out"

    def env(self, extra: dict | None) -> dict:
        e = {"PATH": f"{self.bin}:/usr/bin:/bin", "FAKE_ADB_LOG": str(self.adb_log), "FAKE_CONFIG": str(self.config),
             "FAKE_STREAM": str(self.stream), "FAKE_PUSHED": str(self.pushed), "NEVR_WORK_ROOT": str(self.tmp / "root"),
             "FAKE_LOGCAT_PID": str(self.tmp / "logcat.pid"), "HOME": str(self.tmp)}
        e.update(extra or {})
        return e

    def argv(self, *extra: str) -> list[str]:
        return [sys.executable, "-I", str(RUNNER), "--route", str(self.route), "--out", str(self.out), *extra]

    def go(self, stream: str, *extra: str, env: dict | None = None) -> subprocess.CompletedProcess:
        self.stream.write_text(stream)
        return subprocess.run(self.argv(*extra), capture_output=True, text=True, env=self.env(env), timeout=120)

    def logcat_is_gone(self) -> bool:
        """The fake logcat the runner started no longer runs."""
        pid = int((self.tmp / "logcat.pid").read_text())
        try:
            os.kill(pid, 0)
        except ProcessLookupError:
            return True
        return False

    def tail(self, after: str) -> list[str]:
        """The adb calls after the last call equal to `after`."""
        calls = self.calls()
        return calls[len(calls) - calls[::-1].index(after):]

    def calls(self) -> list[str]:
        return self.adb_log.read_text().splitlines() if self.adb_log.exists() else []

    def records(self) -> list[dict]:
        path = self.out / "runs.jsonl"
        return [json.loads(line) for line in path.read_text().splitlines()] if path.exists() else []

    def last(self, event: str) -> dict:
        found = [r for r in self.records() if r["event"] == event]
        self.assertTrue(found, f"no {event} record in {self.records()}")
        return found[-1]

    def test_a_full_run_passes_and_records_the_window(self):
        p = self.go(LOAD + LOADED + SEAM + REGISTERED + START_LINE + STEPS + end_line())
        self.assertEqual(p.returncode, 0, p.stdout + p.stderr)
        calls = self.calls()
        order = ["shell cat " + FILES + "/config.yaml", f"push {self.route} {ROUTE}", f"shell rm -f {START}",
                 "shell am broadcast -a com.oculus.vrpowermanager.prox_close", "shell am force-stop com.readyatdawn.r15",
                 "logcat -c", "shell am start -n com.readyatdawn.r15/com.oculus.gles3jni.MainActivity", "logcat -v epoch",
                 "shell am force-stop com.readyatdawn.r15",
                 "shell am broadcast -a com.oculus.vrpowermanager.automation_disable"]
        self.assertEqual(calls[:len(order)], order)
        self.assertEqual(calls[len(order)].split()[:2], ["pull", f"{MEDIA_FILES}/nevr-sentinel.log"])
        self.assertEqual(len([c for c in calls if c.startswith("pull ")]), 1)  # the media folder answered
        self.assertEqual(self.pushed.read_text(), self.route.read_text())
        self.assertTrue(self.logcat_is_gone())
        records = self.records()
        for r in records:
            self.assertEqual(datetime.datetime.fromisoformat(r["ts"]).tzinfo, datetime.timezone.utc)
        # Every record is also printed, as written.
        self.assertEqual([json.loads(line) for line in p.stdout.splitlines()], records)
        self.assertEqual(records[-1]["event"], "run_end")
        adb = [r for r in records if r["event"] == "adb"]
        self.assertEqual(adb[0]["args"], ["shell", "cat", f"{FILES}/config.yaml"])
        self.assertEqual((adb[0]["result"], adb[0]["rc"]), ("ok", 0))
        self.assertIn("nevr_autopilot", adb[0]["output"])
        config = self.last("config")
        self.assertEqual((config["named"], config["route"], config["start_file"]), (True, ROUTE, START))
        self.assertEqual(self.last("input_seam")["status"], "ok")
        self.assertEqual(self.last("plugin_load")["outcome"], "loaded")
        self.assertEqual(self.last("route_loaded")["drives"], "left+right")
        self.assertEqual(len({r["run_id"] for r in records}), 1)
        start = self.last("run_start")
        self.assertEqual(len(start["route_sha256"]), 64)
        self.assertEqual(self.last("device")["serial"], SERIAL)
        self.assertEqual([r["index"] for r in records if r["event"] == "route_step"], ["0", "1"])
        end = self.last("run_end")
        self.assertEqual(end["result"], "pass")
        self.assertEqual(end["outcome"], "route_end")
        self.assertEqual(end["route_start"]["realtime_ms"], "1793212224600")
        self.assertEqual(end["route_end"]["realtime_ms"], "1793212226600")
        self.assertEqual(end["route_end"]["measured_s"], "2.011000000")
        self.assertEqual(end["input_seam"]["status"], "ok")
        self.assertEqual(end["plugin_load"]["outcome"], "loaded")
        self.assertEqual(end["steps_logged"], 2)
        run_dir = pathlib.Path(start["out"])
        self.assertIn("route_end", (run_dir / "logcat.txt").read_text())
        self.assertTrue((run_dir / "nevr-sentinel.log").exists())

    def test_the_android_data_folder_is_the_fallback_for_the_sentinel_log(self):
        p = self.go(LOAD + LOADED + SEAM + REGISTERED + START_LINE + STEPS + end_line(),
                    env={"FAKE_FAIL": f"pull {MEDIA_FILES}"})
        self.assertEqual(p.returncode, 0, p.stdout + p.stderr)
        pulls = [c.split()[1] for c in self.calls() if c.startswith("pull ")]
        self.assertEqual(pulls, [f"{MEDIA_FILES}/nevr-sentinel.log", f"{FILES}/nevr-sentinel.log"])
        self.assertTrue((pathlib.Path(self.last("run_start")["out"]) / "nevr-sentinel.log").exists())

    def test_a_burst_is_read_without_waiting_for_more_output(self):
        # The whole stream arrives at once and logcat then stays open and quiet; the run still ends at route_end.
        started = time.monotonic()
        p = self.go(LOADED + REGISTERED + START_LINE + STEPS + end_line(), "--timeout", "30")
        self.assertEqual(p.returncode, 0, p.stdout + p.stderr)
        self.assertEqual(self.last("run_end")["outcome"], "route_end")
        self.assertLess(time.monotonic() - started, 8)

    def test_a_logcat_that_ends_fails_the_run(self):
        p = self.go(LOADED + REGISTERED + START_LINE, env={"FAKE_HOLD": "0"})
        self.assertEqual(p.returncode, 1, p.stdout + p.stderr)
        self.assertEqual(self.last("run_end")["outcome"], "logcat_ended")

    def test_a_driven_hand_with_no_frames_fails(self):
        p = self.go(LOADED + REGISTERED + START_LINE + end_line(left=120, right=0))
        self.assertEqual(p.returncode, 1, p.stdout + p.stderr)
        end = self.last("run_end")
        self.assertEqual(end["result"], "fail")
        self.assertEqual(end["reasons"], ["the route drives the right hand and frames_driven_right is 0"])
        self.assertEqual(self.calls()[-3:-1], ["shell am force-stop com.readyatdawn.r15",
                                               "shell am broadcast -a com.oculus.vrpowermanager.automation_disable"])

    def test_a_hand_the_route_does_not_drive_may_have_no_frames(self):
        loaded = LOADED.replace("drives=left+right", "drives=left")
        p = self.go(loaded + REGISTERED + START_LINE + end_line(left=50, right=0))
        self.assertEqual(p.returncode, 0, p.stdout + p.stderr)

    def test_the_left_hand_is_checked_too(self):
        p = self.go(LOADED + REGISTERED + START_LINE + end_line(left=0, right=50))
        self.assertEqual(p.returncode, 1, p.stdout + p.stderr)
        self.assertEqual(self.last("run_end")["reasons"], ["the route drives the left hand and frames_driven_left is 0"])

    def test_no_route_loaded_line_fails(self):
        p = self.go(REGISTERED + START_LINE + end_line())
        self.assertEqual(p.returncode, 1, p.stdout + p.stderr)
        self.assertEqual(self.last("run_end")["reasons"], ["no route_loaded line was seen"])

    def test_a_refused_route_ends_the_run(self):
        p = self.go(plugin("route_refused", f"route={ROUTE} line=3 reason=\"unknown control 'wheel'\"") + end_line(),
                    "--timeout", "60")
        self.assertEqual(p.returncode, 1, p.stdout + p.stderr)
        end = self.last("run_end")
        self.assertEqual(end["outcome"], "route_refused")
        self.assertEqual(self.last("route_refused")["reason"], "unknown control 'wheel'")
        self.assertIn("shell am broadcast -a com.oculus.vrpowermanager.automation_disable", self.calls())

    def test_a_refused_filter_ends_the_run(self):
        p = self.go(LOADED + plugin("input_filter", "result=not_dev_build") + end_line(), "--timeout", "60")
        self.assertEqual(p.returncode, 1, p.stdout + p.stderr)
        self.assertEqual(self.last("run_end")["outcome"], "input_filter_refused")

    def test_a_plugin_the_loader_refused_ends_the_run(self):
        refused = host({"event": "plugin_load", "name": "nevr_autopilot", "outcome": "refused"})
        p = self.go(refused + LOADED + REGISTERED + START_LINE + end_line(), "--timeout", "60")
        self.assertEqual(p.returncode, 1, p.stdout + p.stderr)
        self.assertEqual(self.last("run_end")["outcome"], "plugin_not_loaded")

    def test_no_end_is_a_timeout(self):
        p = self.go(LOADED + REGISTERED + START_LINE, "--timeout", "2")
        self.assertEqual(p.returncode, 1, p.stdout + p.stderr)
        end = self.last("run_end")
        self.assertEqual(end["outcome"], "timeout")
        self.assertEqual(end["reasons"], ["the run ended with timeout, not route_end"])
        self.assertEqual(len([r for r in self.records() if r["event"] == "route_start"]), 1)  # each line is read once
        self.assertEqual(self.tail("logcat -v epoch")[:2], ["shell am force-stop com.readyatdawn.r15",
                                                            "shell am broadcast -a com.oculus.vrpowermanager.automation_disable"])
        self.assertTrue(self.logcat_is_gone())

    def test_a_launch_the_os_blocks_ends_the_run_at_once(self):
        # Horizon OS's launch check, as a Quest 2 with no controller connected logged it for this app (#1202).
        blocked = ("         1793218573.325  1550  3832 I ActivityLaunchInterceptorController: "
                   "RequiresControllersLaunchInterceptor: com.readyatdawn.r15/com.oculus.gles3jni.MainActivity "
                   "com.oculus.vrshell/com.oculus.vrshell.systemdialog.launchcheck."
                   "LaunchCheckControllerRequiredDialogActivity\n")
        started = time.monotonic()
        p = self.go(blocked + LOADED + REGISTERED + START_LINE + end_line(), "--timeout", "60")
        self.assertLess(time.monotonic() - started, 30)
        self.assertEqual(p.returncode, 1, p.stdout + p.stderr)
        end = self.last("run_end")
        self.assertEqual(end["outcome"], "launch_blocked")
        self.assertEqual(end["reasons"], ["the run ended with launch_blocked, not route_end"])
        self.assertIn("RequiresControllersLaunchInterceptor", self.last("launch_blocked")["line"])
        self.assertIsNone(end["route_loaded"])
        self.assertEqual(self.tail("logcat -v epoch")[:2], ["shell am force-stop com.readyatdawn.r15",
                                                            "shell am broadcast -a com.oculus.vrpowermanager.automation_disable"])

    def test_another_apps_launch_check_is_not_this_runs(self):
        other = ("  1.0  1550  3832 I ActivityLaunchInterceptorController: RequiresControllersLaunchInterceptor: "
                 "com.example.other/.Main com.oculus.vrshell/.systemdialog.launchcheck."
                 "LaunchCheckControllerRequiredDialogActivity\n")
        p = self.go(other + LOAD + LOADED + SEAM + REGISTERED + START_LINE + STEPS + end_line())
        self.assertEqual(p.returncode, 0, p.stdout + p.stderr)
        self.assertEqual(self.last("run_end")["outcome"], "route_end")

    def test_start_on_touches_the_start_file_at_the_match(self):
        stream = (LOADED + REGISTERED + "  1.0  1  1 I EchoVR : lobby loaded\n" + "  1.1  1  1 I EchoVR : lobby loaded\n" +
                  START_LINE + end_line())
        p = self.go(stream, "--start-on", "lobby load")
        self.assertEqual(p.returncode, 0, p.stdout + p.stderr)
        calls = self.calls()
        self.assertIn(f"shell touch {START}", calls)
        self.assertLess(calls.index("logcat -v epoch"), calls.index(f"shell touch {START}"))
        self.assertEqual(calls.count(f"shell touch {START}"), 1)
        self.assertIn("lobby loaded", self.last("start_file_touched")["matched"])

    def test_without_start_on_the_start_file_is_never_touched(self):
        p = self.go(LOADED + REGISTERED + "  1.0  1  1 I EchoVR : lobby loaded\n" + START_LINE + end_line())
        self.assertEqual(p.returncode, 0, p.stdout + p.stderr)
        self.assertFalse([c for c in self.calls() if c.startswith("shell touch")])

    def setup_error(self, *extra: str, env: dict | None = None) -> dict:
        """A run stopped before the sensor was touched: no push, no broadcast, no launch."""
        p = self.go(LOADED, *extra, env=env)
        self.assertEqual(p.returncode, 2, p.stdout + p.stderr)
        self.assertFalse([c for c in self.calls() if c.startswith(("shell am", "push"))])
        end = self.last("run_end")
        self.assertEqual(end["result"], "setup_error")
        self.assertEqual(self.records()[-1]["event"], "run_end")
        return end

    def test_a_config_that_does_not_name_the_plugin_stops_before_anything(self):
        self.config.write_text("plugins:\n  - name: other_plugin\n")
        self.assertEqual(self.setup_error()["reason"], f"{FILES}/config.yaml has no enabled plugins entry named nevr_autopilot")

    def test_a_disabled_entry_stops(self):
        self.config.write_text(CONFIG.replace("enabled: true", "enabled: false"))
        self.assertIn("no enabled plugins entry", self.setup_error()["reason"])

    def test_an_entry_without_a_route_stops(self):
        self.config.write_text("plugins:\n  - name: nevr_autopilot\n    args:\n      start_file: /x\n")
        self.assertEqual(self.setup_error()["reason"], f"the nevr_autopilot entry in {FILES}/config.yaml has no route arg")

    def test_start_on_without_a_start_file_stops(self):
        self.config.write_text(f"plugins:\n  - name: nevr_autopilot\n    args:\n      route: {ROUTE}\n")
        self.assertEqual(self.setup_error("--start-on", "x")["reason"],
                         f"--start-on needs a start_file arg in the nevr_autopilot entry of {FILES}/config.yaml")

    def test_no_headset_stops(self):
        self.assertEqual(self.setup_error(env={"FAKE_DEVICES": ""})["reason"],
                         "exactly one headset must be attached; found 0: []")

    def test_two_headsets_need_a_serial(self):
        two = f"{SERIAL}\tdevice\nOTHER\tdevice\n"
        self.assertIn("found 2", self.setup_error(env={"FAKE_DEVICES": two})["reason"])
        os.remove(self.adb_log) if self.adb_log.exists() else None
        p = self.go(LOADED + REGISTERED + START_LINE + end_line(), "--serial", SERIAL, env={"FAKE_DEVICES": two})
        self.assertEqual(p.returncode, 0, p.stdout + p.stderr)

    def test_a_serial_that_is_not_attached_stops(self):
        self.assertEqual(self.setup_error("--serial", "NOPE")["reason"],
                         f"headset NOPE is not attached (attached: ['{SERIAL}'])")
        self.assertEqual(self.setup_error("--serial", "NOPE", env={"FAKE_DEVICES": ""})["reason"],
                         "headset NOPE is not attached (attached: none)")

    def test_an_unreadable_route_file_stops(self):
        self.route.unlink()
        p = self.go(LOADED)
        self.assertEqual(p.returncode, 2, p.stdout + p.stderr)
        self.assertEqual(self.calls(), [])
        self.assertIn("cannot read the route file", self.last("run_end")["reason"])


    def stopped_after(self, *extra: str, env: dict, stream: str | None = None) -> dict:
        p = self.go(stream or LOADED + REGISTERED + START_LINE + end_line(), *extra, env=env)
        self.assertEqual(p.returncode, 2, p.stdout + p.stderr)
        self.assertEqual(self.records()[-1]["event"], "run_end")
        return self.last("run_end")

    def test_a_failed_adb_call_stops_the_run(self):
        end = self.stopped_after(env={"FAKE_FAIL": "push"})
        self.assertEqual(end["reason"], f"adb push {self.route} {ROUTE} failed (1): error: injected failure")
        failed = [r for r in self.records() if r["event"] == "adb" and r["result"] == "failed"]
        self.assertEqual([(r["args"][0], r["rc"]) for r in failed], [("push", 1)])
        self.assertFalse([c for c in self.calls() if c.startswith("shell am")])  # the sensor was never touched

    def test_a_failure_after_the_override_gives_the_sensor_back(self):
        self.stopped_after(env={"FAKE_FAIL": "logcat -c"})
        self.assertEqual(self.tail("logcat -c"), ["shell am broadcast -a com.oculus.vrpowermanager.automation_disable"])

    def test_a_failure_after_the_launch_stops_the_game(self):
        stream = LOADED + REGISTERED + "  1.0  1  1 I EchoVR : lobby loaded\n" + START_LINE + end_line()
        self.stopped_after("--start-on", "lobby load", env={"FAKE_FAIL": "shell touch"}, stream=stream)
        self.assertEqual(self.tail(f"shell touch {START}"), ["shell am force-stop com.readyatdawn.r15",
                                                            "shell am broadcast -a com.oculus.vrpowermanager.automation_disable"])
        self.assertTrue(self.logcat_is_gone())

    def test_an_adb_call_that_hangs_times_out(self):
        end = self.stopped_after("--adb-timeout", "2", env={"FAKE_SLOW": "push"})
        self.assertEqual(end["reason"], f"adb push {self.route} {ROUTE} timed out after 2 s")
        timed = [r for r in self.records() if r["event"] == "adb" and r["result"] == "timeout"]
        self.assertEqual([(r["args"][0], r["timeout_s"]) for r in timed], [("push", 2)])

    def test_a_cleanup_call_that_hangs_does_not_stop_the_record(self):
        p = self.go(LOADED + REGISTERED + START_LINE + end_line(), "--adb-timeout", "2",
                    env={"FAKE_SLOW": "shell am broadcast -a com.oculus.vrpowermanager.automation_disable"})
        self.assertEqual(p.returncode, 0, p.stdout + p.stderr)
        self.assertEqual(self.last("run_end")["result"], "pass")
        self.assertEqual(self.calls()[-1].split()[0], "pull")  # the run went on after the hung call

    def test_an_interrupt_stops_the_game_and_gives_the_sensor_back(self):
        self.stream.write_text(LOADED + REGISTERED + START_LINE)
        proc = subprocess.Popen(self.argv(), stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
                                env=self.env(None))
        for line in proc.stdout:
            if json.loads(line)["event"] == "route_start":
                break
        proc.send_signal(signal.SIGINT)
        proc.communicate(timeout=60)
        self.assertEqual(proc.returncode, 130)
        end = self.last("run_end")
        self.assertEqual((end["result"], end["reason"]), ("interrupted", "interrupted (SIGINT) before the run ended"))
        self.assertEqual(self.tail("logcat -v epoch"), ["shell am force-stop com.readyatdawn.r15",
                                                        "shell am broadcast -a com.oculus.vrpowermanager.automation_disable"])
        self.assertTrue(self.logcat_is_gone())


class ParseTest(unittest.TestCase):
    def test_plugin_lines(self):
        self.assertEqual(run.parse_plugin_line(plugin("route_refused", 'line=3 reason="a \\"b\\" c" route=')),
                         ("route_refused", {"line": "3", "reason": 'a "b" c', "route": ""}))
        self.assertIsNone(run.parse_plugin_line("[NEVR.PLUGIN] plugin=other route_end name=x"))
        self.assertIsNone(run.parse_plugin_line("I EchoVR : route_end"))

    def test_config_paths(self):
        self.assertEqual(run.config_paths(CONFIG), {"named": True, "route": ROUTE, "start_file": START})
        self.assertEqual(run.config_paths("plugins:\n  - name: nevr_autopilot2\n")["named"], False)
        self.assertEqual(run.config_paths("plugins:\n  - name: 'nevr_autopilot'\n    args:\n      route: /r\n"),
                         {"named": True, "route": "/r", "start_file": None})
        # The next entry's args are not this entry's.
        self.assertEqual(run.config_paths("plugins:\n  - name: nevr_autopilot\n  - name: b\n    args:\n      route: /b\n"),
                         {"named": True, "route": None, "start_file": None})
        # Blank and comment lines inside an entry, at any indent, do not end it.
        self.assertEqual(run.config_paths("plugins:\n  - name: nevr_autopilot\n\n# note\n    args:\n      route: /r\n"),
                         {"named": True, "route": "/r", "start_file": None})
        # The first enabled entry is the one the loader runs.
        two = ("plugins:\n  - name: nevr_autopilot\n    enabled: false\n    args:\n      route: /off\n"
               "  - name: nevr_autopilot\n    args:\n      route: /on\n  - name: nevr_autopilot\n    args:\n      route: /dup\n")
        self.assertEqual(run.config_paths(two), {"named": True, "route": "/on", "start_file": None})

    def test_host_records(self):
        self.assertEqual(run.parse_host_record('  1.0  1  1 I NEVR-Sentinel: {"event":"input_seam","result":"installed"}'),
                         {"event": "input_seam", "result": "installed"})
        self.assertEqual(run.parse_host_record("  1.0  1  1 I NEVR-Sentinel: no record here"), {})
        self.assertEqual(run.parse_host_record("  1.0  1  1 I NEVR-Sentinel: {not json"), {})


if __name__ == "__main__":
    unittest.main()
