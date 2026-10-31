#!/usr/bin/env python3
"""One unattended autopilot run on a Quest over adb (#1202, docs/adr/0025-quest-autopilot-route.md).

The headset needs a development APK that carries nevr_autopilot (`just quest-autopilot-apk`) and a config.yaml in the
game's files directory whose `plugins:` list names the plugin with `route` (and, for --start-on, `start_file`) args.
The runner never writes that config: it reads it and stops when it does not name the plugin and the paths.

What it does, in order:
  1. checks that exactly one headset is attached (or --serial names one), and reads config.yaml;
  2. pushes the route file to the `route` path and removes a stale start file;
  3. overrides the proximity sensor (`am broadcast -a com.oculus.vrpowermanager.prox_close`), force-stops the game,
     clears logcat and starts the game;
  4. reads logcat until the plugin logs route_end, the plugin refuses (route_refused, a failed input_filter, a failed
     plugin_load), Horizon OS blocks the launch (launch_blocked: no controller is connected), or --timeout passes;
     with --start-on REGEX it creates the start file at the first logcat line the regex matches;
  5. force-stops the game, gives the sensor back (`automation_disable`), and saves the run's logcat and
     nevr-sentinel.log.

Every step is a JSON record in <out>/runs.jsonl (default: quest-autopilot/ under the work root), with a run id, the
headset serial, the route's sha256, and the plugin's route_start/route_end fields in the summary record. The exit code is
0 only when route_end arrived and each hand the route drives (route_loaded `drives`) has frames_driven above 0; 1 when the
run completed without that; 2 on a setup error (no headset, a config that does not name the plugin, an adb failure);
130 when interrupted (SIGINT). The game is stopped and the sensor given back on every way out once they were touched.

usage: tools/quest_autopilot_run.py --route FILE [--serial S] [--adb PATH] [--out DIR] [--start-on REGEX]
                                    [--timeout SECONDS] [--adb-timeout SECONDS]
"""
from __future__ import annotations

import argparse
import datetime
import hashlib
import json
import os
import pathlib
import queue
import re
import subprocess
import sys
import threading
import time
import uuid

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import work_root  # noqa: E402

PACKAGE = "com.readyatdawn.r15"
ACTIVITY = f"{PACKAGE}/com.oculus.gles3jni.MainActivity"
FILES = f"/sdcard/Android/data/{PACKAGE}/files"
CONFIG = f"{FILES}/config.yaml"
# The runtime writes nevr-sentinel.log in the Android/media files folder; the Android/data one is read when an older
# build left it there.
MEDIA_FILES = f"/sdcard/Android/media/{PACKAGE}/files"
SENTINEL_LOGS = (f"{MEDIA_FILES}/nevr-sentinel.log", f"{FILES}/nevr-sentinel.log")
PLUGIN = "nevr_autopilot"
PROX_CLOSE = "com.oculus.vrpowermanager.prox_close"
PROX_RESTORE = "com.oculus.vrpowermanager.automation_disable"
# Horizon OS refuses to start an app that requires controllers while none is connected; the game never runs (#1202).
LAUNCH_BLOCKED = re.compile(rf"RequiresControllersLaunchInterceptor: {re.escape(PACKAGE)}/")
PLUGIN_LINE = re.compile(r"\[NEVR\.PLUGIN\] plugin=(\S+) (\S+)(.*)$")
FIELD = re.compile(r'(\w+)=("(?:[^"\\]|\\.)*"|\S*)')


class SetupError(Exception):
    """A run that could not start: no headset, a config that does not name the plugin, an adb failure."""


def now_iso() -> str:
    return datetime.datetime.now(datetime.timezone.utc).isoformat(timespec="milliseconds")


class Recorder:
    """runs.jsonl: one JSON object per line, flushed as it is written. A write that fails raises (loud)."""

    def __init__(self, path: pathlib.Path, run_id: str):
        self.path = path
        self.run_id = run_id

    def write(self, event: str, **fields) -> dict:
        record = {"ts": now_iso(), "run_id": self.run_id, "event": event, **fields}
        with open(self.path, "a", encoding="utf-8") as handle:
            handle.write(json.dumps(record, sort_keys=True) + "\n")
        print(json.dumps(record, sort_keys=True), flush=True)


def parse_plugin_line(line: str) -> tuple[str, dict] | None:
    """The message and fields of a `[NEVR.PLUGIN] plugin=nevr_autopilot <message> k=v ...` line; None for any other."""
    m = PLUGIN_LINE.search(line)
    if not m or m.group(1) != PLUGIN:
        return None
    fields = {}
    for key, value in FIELD.findall(m.group(3)):
        if value.startswith('"'):
            value = re.sub(r"\\(.)", r"\1", value[1:-1])
        fields[key] = value
    return m.group(2), fields


def parse_host_record(line: str) -> dict:
    """The JSON object of a sentinel line (`NEVR-Sentinel: {...}`), or {} for a line without one."""
    try:
        return json.loads(line[line.index("{"):])
    except ValueError:  # no "{" (str.index), or not JSON (json.JSONDecodeError is a ValueError)
        return {}


def config_paths(config: str) -> dict:
    """What config.yaml says of the plugin: whether an enabled entry names it, and that entry's route and start_file
    args. The first enabled entry is the one the loader runs (a later one naming the same plugin is refused as a
    duplicate). A plain reading of the plugins list, enough to stop a run that cannot work; the sentinel's own parser is
    the authority."""
    lines = config.splitlines()
    for i, line in enumerate(lines):
        if not re.match(r"^\s*-\s*name:\s*['\"]?" + PLUGIN + r"['\"]?\s*(#.*)?$", line):
            continue
        entry = {"named": True, "route": None, "start_file": None}
        indent = len(line) - len(line.lstrip())
        for follow in lines[i + 1:]:
            stripped = follow.strip()
            if not stripped or stripped.startswith("#"):
                continue
            if len(follow) - len(follow.lstrip()) <= indent:
                break
            m = re.match(r"^\s*(route|start_file|enabled):\s*['\"]?([^'\"#]*?)['\"]?\s*(#.*)?$", follow)
            if m and m.group(1) == "enabled" and m.group(2).strip().lower() == "false":
                entry["named"] = False
            elif m and m.group(1) in ("route", "start_file"):
                entry[m.group(1)] = m.group(2).strip()
        if entry["named"]:
            return entry
    return {"named": False, "route": None, "start_file": None}


class Adb:
    def __init__(self, adb: str, serial: str | None, recorder: Recorder, timeout: int):
        self.adb = adb
        self.serial = serial
        self.rec = recorder
        self.timeout = timeout

    def base(self) -> list[str]:
        return [self.adb] + (["-s", self.serial] if self.serial else [])

    def run(self, *args: str, check: bool = True) -> str:
        cmd = self.base() + list(args)
        try:
            done = subprocess.run(cmd, capture_output=True, text=True, timeout=self.timeout, stdin=subprocess.DEVNULL)
        except subprocess.TimeoutExpired as e:
            self.rec.write("adb", args=list(args), result="timeout", timeout_s=self.timeout)
            if not check:
                return ""
            raise SetupError(f"adb {' '.join(args)} timed out after {self.timeout} s") from e
        self.rec.write("adb", args=list(args), result="ok" if done.returncode == 0 else "failed", rc=done.returncode,
                       output=(done.stdout + done.stderr).strip()[-400:])
        if check and done.returncode != 0:
            raise SetupError(f"adb {' '.join(args)} failed ({done.returncode}): {(done.stdout + done.stderr).strip()}")
        return done.stdout

    def pick_device(self) -> str:
        out = subprocess.run([self.adb, "devices"], capture_output=True, text=True, timeout=self.timeout,
                             stdin=subprocess.DEVNULL)
        devices = [line.split("\t")[0] for line in out.stdout.splitlines()[1:] if line.endswith("\tdevice")]
        if self.serial:
            if self.serial not in devices:
                raise SetupError(f"headset {self.serial} is not attached (attached: {devices or 'none'})")
            return self.serial
        if len(devices) != 1:
            raise SetupError(f"exactly one headset must be attached; found {len(devices)}: {devices}")
        self.serial = devices[0]
        return self.serial


def watch_lines(adb: Adb, rec: Recorder, save, lines: queue.Queue, deadline: float, start_on: re.Pattern | None,
                start_file: str | None, seen: dict) -> None:
    """The body of watch: reads lines until the run ends, filling `seen`."""
    while time.monotonic() < deadline:
        try:
            line = lines.get(timeout=0.5)
        except queue.Empty:
            continue
        if line is None:
            seen["outcome"] = "logcat_ended"
            return
        save.write(line)
        if start_on is not None and not seen["start_file_touched"] and start_on.search(line):
            adb.run("shell", "touch", start_file)
            seen["start_file_touched"] = True
            rec.write("start_file_touched", start_file=start_file, matched=line.strip()[-300:])
        if LAUNCH_BLOCKED.search(line):
            rec.write("launch_blocked", line=line.strip()[-400:])
            seen["outcome"] = "launch_blocked"
            return
        parsed = parse_plugin_line(line)
        if parsed is None:
            host = parse_host_record(line) if "NEVR-Sentinel" in line else {}
            if host.get("event") == "input_seam":
                seen["input_seam"] = host
                rec.write("input_seam", **{k: v for k, v in host.items() if k != "event"})
            elif host.get("event") == "plugin_load" and host.get("name") == PLUGIN:
                seen["plugin_load"] = host
                rec.write("plugin_load", **{k: v for k, v in host.items() if k != "event"})
                if host.get("outcome") != "loaded":
                    seen["outcome"] = "plugin_not_loaded"
                    return
            continue
        message, fields = parsed
        if message == "route_step":
            seen["steps"] += 1
            rec.write("route_step", **fields)
            continue
        if message in seen:
            seen[message] = fields
        rec.write(message, **fields)
        if message == "route_refused":
            seen["outcome"] = "route_refused"
            return
        if message == "input_filter" and fields.get("result") != "registered":
            seen["outcome"] = "input_filter_refused"
            return
        if message == "route_end":
            seen["outcome"] = "route_end"
            return


def watch(adb: Adb, rec: Recorder, logcat_path: pathlib.Path, deadline: float, start_on: re.Pattern | None,
          start_file: str | None) -> dict:
    """Reads logcat until the run ends. Returns what was seen: the plugin's lines by message, the host's input_seam
    record, the outcome token."""
    seen = {"route_loaded": None, "route_start": None, "route_end": None, "route_refused": None, "input_filter": None,
            "input_seam": None, "plugin_load": None, "steps": 0, "outcome": "timeout", "start_file_touched": False}
    # A reader thread, not select(): readline() reads ahead into the file object's buffer, where select() cannot see
    # the lines of a burst, so they would wait for the next bytes logcat happens to send. None marks the stream's end.
    lines: queue.Queue = queue.Queue()
    with subprocess.Popen(adb.base() + ["logcat", "-v", "epoch"], stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                          stdin=subprocess.DEVNULL, text=True) as proc:

        def read() -> None:
            try:
                for got in proc.stdout:
                    lines.put(got)
            except (OSError, ValueError):  # the stream was closed under it: the run is over either way
                pass
            lines.put(None)

        threading.Thread(target=read, daemon=True).start()
        try:
            with open(logcat_path, "w", encoding="utf-8") as save:
                watch_lines(adb, rec, save, lines, deadline, start_on, start_file, seen)
        finally:
            proc.kill()  # leaving the block waits for it
    return seen


def verdict(seen: dict) -> tuple[int, list[str]]:
    """0 and no reasons when the route ended and every hand it drives was driven; else 1 and why."""
    if seen["outcome"] != "route_end":
        return 1, [f"the run ended with {seen['outcome']}, not route_end"]
    drives = (seen["route_loaded"] or {}).get("drives", "")
    end = seen["route_end"] or {}
    reasons = []
    for hand in ("left", "right"):
        if hand in drives.split("+") and int(end.get(f"frames_driven_{hand}", "0") or 0) <= 0:
            reasons.append(f"the route drives the {hand} hand and frames_driven_{hand} is {end.get(f'frames_driven_{hand}')}")
    if not seen["route_loaded"]:
        reasons.append("no route_loaded line was seen")
    return (1 if reasons else 0), reasons


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--route", required=True, type=pathlib.Path, help="the route file to push and run")
    ap.add_argument("--serial", help="the adb serial of the headset (default: the one attached)")
    ap.add_argument("--adb", default="adb", help="the adb to use (default: adb on the PATH)")
    ap.add_argument("--out", type=pathlib.Path, help="the output directory (default: quest-autopilot/ under the work root)")
    ap.add_argument("--start-on", help="create the start file at the first logcat line this regex matches")
    ap.add_argument("--timeout", type=int, default=900, help="seconds to wait for route_end after the launch (default 900)")
    ap.add_argument("--adb-timeout", type=int, default=60, help="seconds each adb command may take (default 60)")
    args = ap.parse_args(argv)

    out = args.out or work_root.work_root() / "quest-autopilot"
    work_root.ensure_dir(out, "Autopilot runs on a Quest (tools/quest_autopilot_run.py): runs.jsonl, each run's logcat and "
                         "nevr-sentinel.log.", "a run's files when the PR or report citing them is closed; runs.jsonl is kept.")
    run_id = datetime.datetime.now(datetime.timezone.utc).strftime("%Y%m%dT%H%M%SZ") + "-" + uuid.uuid4().hex[:6]
    rec = Recorder(out / "runs.jsonl", run_id)
    run_dir = out / run_id
    run_dir.mkdir(parents=True)
    adb = Adb(args.adb, args.serial, rec, args.adb_timeout)
    start_on = re.compile(args.start_on) if args.start_on else None
    try:
        route_bytes = args.route.read_bytes()
    except OSError as e:
        rec.write("run_end", result="setup_error", reason=f"cannot read the route file: {e}")
        return 2
    rec.write("run_start", route=str(args.route), route_sha256=hashlib.sha256(route_bytes).hexdigest(),
              start_on=args.start_on, timeout_s=args.timeout, out=str(run_dir))
    overridden = launched = False
    stopped = None  # (result, reason, exit code) when the run stops before it is judged
    try:
        serial = adb.pick_device()
        rec.write("device", serial=serial)
        cfg = config_paths(adb.run("shell", "cat", CONFIG))
        rec.write("config", config=CONFIG, **cfg)
        if not cfg["named"]:
            raise SetupError(f"{CONFIG} has no enabled plugins entry named {PLUGIN}")
        if not cfg["route"]:
            raise SetupError(f"the {PLUGIN} entry in {CONFIG} has no route arg")
        if start_on is not None and not cfg["start_file"]:
            raise SetupError(f"--start-on needs a start_file arg in the {PLUGIN} entry of {CONFIG}")
        adb.run("push", str(args.route), cfg["route"])
        if cfg["start_file"]:
            adb.run("shell", "rm", "-f", cfg["start_file"])
        overridden = True  # from the broadcast on, the sensor is given back on every way out
        adb.run("shell", "am", "broadcast", "-a", PROX_CLOSE)
        adb.run("shell", "am", "force-stop", PACKAGE)
        adb.run("logcat", "-c")
        adb.run("shell", "am", "start", "-n", ACTIVITY)
        launched = True
        seen = watch(adb, rec, run_dir / "logcat.txt", time.monotonic() + args.timeout, start_on, cfg["start_file"])
    except SetupError as e:
        stopped = ("setup_error", str(e), 2)
    except KeyboardInterrupt:
        stopped = ("interrupted", "interrupted (SIGINT) before the run ended", 130)
    finally:
        # Whatever ends the run, the game is stopped and the sensor given back.
        if launched:
            adb.run("shell", "am", "force-stop", PACKAGE, check=False)
        if overridden:
            adb.run("shell", "am", "broadcast", "-a", PROX_RESTORE, check=False)
    if stopped is not None:
        rec.write("run_end", result=stopped[0], reason=stopped[1])
        return stopped[2]
    for remote in SENTINEL_LOGS:
        adb.run("pull", remote, str(run_dir / "nevr-sentinel.log"), check=False)
        if (run_dir / "nevr-sentinel.log").exists():
            break
    code, reasons = verdict(seen)
    rec.write("run_end", result="pass" if code == 0 else "fail", reasons=reasons, outcome=seen["outcome"],
              steps_logged=seen["steps"], route_loaded=seen["route_loaded"], route_start=seen["route_start"],
              route_end=seen["route_end"], input_filter=seen["input_filter"], input_seam=seen["input_seam"],
              plugin_load=seen["plugin_load"])
    return code


if __name__ == "__main__":
    sys.exit(main())
