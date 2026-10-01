#!/usr/bin/env python3
"""Run one social scenario end to end with nobody touching anything.

Design: docs/design/2026-10-01-social-scenario-harness.md. The client is launched exactly as
launch-client.sh launches it (nested display :101, pristine install and restore, production login),
with the mingw-scenario build of BugSplat64.dll, which carries the test-only control endpoint
(src/runtime/scenario/scenario_control.cpp). Steps come from a YAML file; each one either waits
for a line in the game's console log, talks to the control endpoint, or expects a line after the
previous action. The first failing step stops the run and is named. Exit 0 only if every step passed.

Usage:
  tools/scenario/run_scenario.py tools/scenario/scenarios/invite.yaml [--dll PATH] [--out DIR]
"""
from __future__ import annotations

import argparse
import datetime
import json
import os
import pathlib
import re
import shutil
import signal
import socket
import string
import subprocess
import sys
import time

import yaml

REPO = pathlib.Path(__file__).resolve().parents[2]
DEFAULT_DLL = REPO / "build/mingw-scenario/bin/BugSplat64.dll"
SCRATCH = pathlib.Path("/var/tmp/work-nevr-runtime/scenario-runs")
WINEPREFIX = REPO / "echovr/.wineprefix"
MARKER = b"[NEVR.SCENARIO]"
ANSI = re.compile(r"\x1b\[[0-9;]*m")
STEP_KINDS = ("wait_log", "expect_log", "state_until", "inject", "fire")


class StepFailed(Exception):
    pass


def substitute(value, variables: dict):
    """Expands ${name} in strings (recursively); a string that is exactly one variable keeps the
    variable's type, so `id: ${friend_id}` stays an integer."""
    if isinstance(value, str):
        whole = re.fullmatch(r"\$\{(\w+)\}", value)
        if whole:
            return variables[whole.group(1)]
        return string.Template(value).substitute(variables)
    if isinstance(value, dict):
        return {k: substitute(v, variables) for k, v in value.items()}
    if isinstance(value, list):
        return [substitute(v, variables) for v in value]
    return value


def load_scenario(path: pathlib.Path) -> dict:
    data = yaml.safe_load(path.read_text())
    if not isinstance(data, dict) or "steps" not in data or "name" not in data:
        raise ValueError(f"{path}: a scenario needs 'name' and 'steps'")
    variables = {k: v for k, v in (data.get("vars") or {}).items()}
    steps = []
    for i, raw in enumerate(data["steps"]):
        kinds = [k for k in STEP_KINDS if k in raw]
        if "name" not in raw or len(kinds) != 1:
            raise ValueError(f"{path}: step {i + 1} needs a name and exactly one of {', '.join(STEP_KINDS)}")
        step = substitute(raw, variables)
        step["kind"] = kinds[0]
        steps.append(step)
    return {"name": data["name"], "description": data.get("description", ""), "steps": steps}


class ConsoleLog:
    """The game's console output, read fresh on every poll; positions are character offsets."""

    def __init__(self, path: pathlib.Path):
        self.path = path

    def text(self) -> str:
        try:
            return ANSI.sub("", self.path.read_text(errors="replace"))
        except FileNotFoundError:
            return ""

    def wait(self, pattern: str, timeout: float, start: int = 0) -> re.Match | None:
        regex = re.compile(pattern)
        deadline = time.monotonic() + timeout
        while True:
            match = regex.search(self.text(), start)
            if match or time.monotonic() >= deadline:
                return match
            time.sleep(0.5)


class Control:
    def __init__(self, port: int):
        self.sock = socket.create_connection(("127.0.0.1", port), timeout=10)
        self.buffer = b""

    def call(self, command: dict) -> dict:
        self.sock.sendall((json.dumps(command) + "\n").encode())
        while b"\n" not in self.buffer:
            chunk = self.sock.recv(65536)
            if not chunk:
                raise StepFailed("control endpoint closed the connection")
            self.buffer += chunk
        line, self.buffer = self.buffer.split(b"\n", 1)
        return json.loads(line)

    def close(self):
        self.sock.close()


def state_matches(state: dict, step: dict) -> tuple[bool, str]:
    spec = step["state_until"]
    if "party_joinable" in spec:
        party = state.get("party", {})
        ok = bool(party.get("joinable")) == bool(spec["party_joinable"])
        return ok, f"party id={party.get('id')} joinable={party.get('joinable')} members={party.get('members')}"
    if "friend_invitable" in spec:
        target = int(spec["friend_invitable"])
        row = next((f for f in state.get("friends", []) if int(f["id"]) == target), None)
        if row is None:
            return False, f"friend {target} is not in the roster ({len(state.get('friends', []))} friends)"
        return row["invitable"] == 1, f"friend {target} online={row['online']} invitable={row['invitable']}"
    raise StepFailed(f"state_until has no known check: {sorted(spec)}")


class Run:
    def __init__(self, scenario: dict, dll: pathlib.Path, out: pathlib.Path):
        self.scenario, self.dll, self.out = scenario, dll, out
        self.results: list[dict] = []
        self.console: ConsoleLog | None = None
        self.control: Control | None = None
        self.mark = 0  # console offset at the last action; expect_log looks after it
        self.launcher: subprocess.Popen | None = None
        self.xephyr: subprocess.Popen | None = None

    # -- environment -------------------------------------------------------------------------------
    @staticmethod
    def display_ready() -> bool:
        # The socket file appears before the server accepts clients; Wine then fails with "no driver
        # could be loaded" (measured 2026-10-01, run 20261001T142307-invite). Ready = a client connects.
        env = {k: v for k, v in os.environ.items() if k != "WAYLAND_DISPLAY"}
        env["DISPLAY"] = ":101"
        return subprocess.run(["xdpyinfo"], env=env, capture_output=True).returncode == 0

    def ensure_xephyr(self):
        if not self.display_ready():
            if subprocess.run(["pgrep", "-f", "Xephyr :101"], capture_output=True).returncode != 0:
                self.xephyr = subprocess.Popen(["Xephyr", ":101", "-screen", "1920x1080"],
                                               stdout=(self.out / "xephyr.log").open("w"),
                                               stderr=subprocess.STDOUT, start_new_session=True)
            deadline = time.monotonic() + 20
            while not self.display_ready():
                if time.monotonic() > deadline or (self.xephyr is not None and self.xephyr.poll() is not None):
                    raise StepFailed(f"display :101 never accepted a client; see {self.out / 'xephyr.log'}")
                time.sleep(0.5)

    def launch(self):
        launcher_out = self.out / "launch-client.out"
        self.launcher = subprocess.Popen(
            [str(REPO / "launch-client.sh"), "--dll", str(self.dll)], cwd=REPO,
            stdout=launcher_out.open("w"), stderr=subprocess.STDOUT, start_new_session=True)
        deadline = time.monotonic() + 30
        while time.monotonic() < deadline:
            m = re.search(r"=== Console log: (\S+) ===", launcher_out.read_text())
            if m:
                self.console = ConsoleLog(pathlib.Path(m.group(1)))
                return
            if self.launcher.poll() is not None:
                raise StepFailed(f"launch-client.sh exited early (rc={self.launcher.returncode}); see {launcher_out}")
            time.sleep(0.3)
        raise StepFailed("launch-client.sh never printed its console log path")

    def teardown(self):
        if self.control:
            self.control.close()
        env = dict(os.environ, WINEPREFIX=str(WINEPREFIX))
        subprocess.run(["wineserver", "-k"], env=env, capture_output=True)
        if self.launcher:
            try:
                self.launcher.wait(timeout=90)
            except subprocess.TimeoutExpired:
                os.killpg(self.launcher.pid, signal.SIGTERM)
                self.launcher.wait(timeout=30)
        if self.xephyr:
            self.xephyr.terminate()
            self.xephyr.wait(timeout=15)

    # -- steps -------------------------------------------------------------------------------------
    def connect(self):
        if self.control:
            return
        m = re.search(r"\[NEVR\.SCENARIO\] control listening on 127\.0\.0\.1:(\d+)", self.console.text())
        if not m:
            raise StepFailed("the control endpoint never logged its port (is this the mingw-scenario DLL?)")
        self.control = Control(int(m.group(1)))

    def do(self, step: dict) -> str:
        kind = step["kind"]
        if kind == "wait_log":
            spec = step["wait_log"]
            m = self.console.wait(spec["pattern"], float(spec.get("timeout", 30)))
            if not m:
                raise StepFailed(f"no line matching /{spec['pattern']}/ within {spec.get('timeout', 30)} s")
            return m.group(0)[:160]
        if kind == "expect_log":
            spec = step["expect_log"]
            m = self.console.wait(spec["pattern"], float(spec.get("timeout", 10)), self.mark)
            if not m:
                found = []
                text = self.console.text()[self.mark:]
                for pattern in step.get("diagnose", []):
                    found += [line.strip()[:200] for line in text.splitlines() if re.search(pattern, line)]
                why = f"no line matching /{spec['pattern']}/ within {spec.get('timeout', 10)} s after the last action"
                if found:
                    why += "; instead: " + " | ".join(found[:4])
                raise StepFailed(why)
            return m.group(0)[:160]
        self.connect()
        if kind == "state_until":
            deadline = time.monotonic() + float(step["state_until"].get("timeout", 15))
            detail = ""
            while time.monotonic() < deadline:
                state = self.control.call({"op": "state"})
                if not state.get("ok"):
                    raise StepFailed(f"state failed: {state.get('error')}")
                ok, detail = state_matches(state, step)
                if ok:
                    return detail
                time.sleep(1)
            raise StepFailed(f"condition not met in time: {detail}")
        command = {"op": kind, **step[kind]}
        self.mark = len(self.console.text())
        reply = self.control.call(command)
        if not reply.get("ok"):
            raise StepFailed(f"{kind} failed: {reply.get('error')}")
        return json.dumps(reply)[:160]

    def execute(self) -> bool:
        started = time.monotonic()
        try:
            self.ensure_xephyr()
            self.launch()
        except StepFailed as exc:
            self.results.append({"step": "launch the client", "result": "FAIL", "seconds": 0, "detail": str(exc)})
            return False
        failed = False
        for step in self.scenario["steps"]:
            if failed:
                self.results.append({"step": step["name"], "result": "SKIP", "seconds": 0, "detail": ""})
                continue
            t0 = time.monotonic()
            try:
                detail = self.do(step)
                result = "PASS"
            except (StepFailed, OSError, json.JSONDecodeError) as exc:
                detail, result, failed = str(exc), "FAIL", True
            self.results.append({"step": step["name"], "result": result,
                                 "seconds": round(time.monotonic() - t0, 1), "detail": detail})
        self.results.append({"step": "(total)", "result": "FAIL" if failed else "PASS",
                             "seconds": round(time.monotonic() - started, 1), "detail": ""})
        return not failed


def table(results: list[dict]) -> str:
    rows = ["| # | step | result | s | detail |", "|---|---|---|---|---|"]
    for i, r in enumerate(results, 1):
        detail = r["detail"].replace("|", "\\|")
        rows.append(f"| {i} | {r['step']} | {r['result']} | {r['seconds']} | {detail} |")
    return "\n".join(rows)


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("scenario", type=pathlib.Path)
    parser.add_argument("--dll", type=pathlib.Path, default=DEFAULT_DLL)
    parser.add_argument("--out", type=pathlib.Path)
    args = parser.parse_args(argv)

    scenario = load_scenario(args.scenario)
    if not args.dll.is_file() or MARKER not in args.dll.read_bytes():
        print(f"ERROR: {args.dll} is not a scenario build (missing or no {MARKER.decode()}); "
              "run `just preset=mingw-scenario build`", file=sys.stderr)
        return 2
    stamp = datetime.datetime.now().strftime("%Y%m%dT%H%M%S")
    out = args.out or SCRATCH / f"{stamp}-{scenario['name']}"
    out.mkdir(parents=True, exist_ok=True)

    run = Run(scenario, args.dll, out)
    try:
        passed = run.execute()
    finally:
        run.teardown()
    if run.console and run.console.path.exists():
        shutil.copy(run.console.path, out / "console.log")
    launcher_text = (out / "launch-client.out").read_text() if (out / "launch-client.out").exists() else ""
    game_log = re.search(r"=== game log: (\S+) ===", launcher_text)
    if game_log and pathlib.Path(game_log.group(1)).exists():
        shutil.copy(game_log.group(1), out / "game.jsonl")
    restored = "original BugSplat64.dll restored and verified" in launcher_text

    report = {"scenario": scenario["name"], "dll": str(args.dll), "passed": passed,
              "dll_restored": restored, "steps": run.results}
    (out / "result.json").write_text(json.dumps(report, indent=2))
    md = f"# scenario {scenario['name']}: {'PASS' if passed else 'FAIL'}\n\n{table(run.results)}\n"
    md += f"\nDLL: {args.dll}\nOriginal BugSplat64.dll restored: {restored}\n"
    (out / "table.md").write_text(md)
    print(md)
    print(f"run folder: {out}")
    if not restored:
        print("WARNING: launch-client.sh did not report restoring the original DLL; check "
              f"{out / 'launch-client.out'}", file=sys.stderr)
        return 1
    return 0 if passed else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
