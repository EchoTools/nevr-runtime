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
STEP_KINDS = ("wait_log", "expect_log", "state_until", "inject", "fire", "nakama_log", "peer")
# Lines in the game's own log that mean the run is over: a wait stops on the first one and reports
# it, instead of sitting out its timeout on a game that already failed (2026-10-01 run
# 20261001T142307 waited 240 s on a client that had died at 0.2 s: "no driver could be loaded").
FATAL_PATTERNS = [
    r"no driver could be loaded",
    r"DirectX error: E_OUTOFMEMORY",
    r"Unknown error while loading the game",
    r"\[NEVR\.CRASH\] HandleCrashDump ENTERED",
    r"\[NEVR\.FATAL\]",
    r"NetGame switching state \(from [^)]*, to (login failed|service unavailable|logged out)\)",
    r"Lost connection to the login service",
]
FATAL = re.compile("|".join(FATAL_PATTERNS))
STATE = re.compile(r"NetGame switching state \(from [^)]*, to [^)]*\)")
NOTABLE = re.compile(r"\b(warn|error)\b.*")


class StepFailed(Exception):
    pass


def substitute(value, variables: dict):
    """Expands ${name} in strings (recursively); a string that is exactly one variable keeps the
    variable's type, so `id: ${friend_id}` stays an integer."""
    if isinstance(value, str):
        whole = re.fullmatch(r"\$\{(\w+)\}", value)
        if whole and whole.group(1) in variables:
            return variables[whole.group(1)]
        # A name not yet known is left for the run: a fire step can `save` a reply field into it.
        return string.Template(value).safe_substitute(variables)
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
    server = data.get("server", "production")
    peers = int(data.get("peers", 0))
    friends = [[str(x) for x in pair] for pair in (data.get("friends") or [])]
    met = [[str(x) for x in pair] for pair in (data.get("met") or [])]
    if server not in ("production", "local"):
        raise ValueError(f"{path}: server must be 'production' or 'local', not {server!r}")
    return {"name": data["name"], "description": data.get("description", ""), "steps": steps, "server": server,
            "peers": peers, "friends": friends, "met": met}


class ConsoleLog:
    """The game's console output, read fresh on every poll; positions are character offsets."""

    def __init__(self, path: pathlib.Path):
        self.path = path

    def text(self) -> str:
        try:
            return ANSI.sub("", self.path.read_text(errors="replace"))
        except FileNotFoundError:
            return ""

    def wait(self, pattern: str, timeout: float | None, start: int = 0, alive=None) -> re.Match | None:
        """The first match of `pattern` after `start`. Raises StepFailed, quoting the log, as soon as
        the game logs a fatal line after `start`, or the game is gone (`alive` returns False); a
        timeout is reported with the game's last state and its last warnings and errors. With no
        timeout it waits as long as the game runs: how long a game takes to load is not a failure."""
        regex = re.compile(pattern)
        deadline = None if timeout is None else time.monotonic() + timeout
        while True:
            text = self.text()
            match = regex.search(text, start)
            if match:
                return match
            fatal = FATAL.search(text, start)
            if fatal:
                line = text[text.rfind("\n", 0, fatal.start()) + 1:text.find("\n", fatal.end())].strip()
                raise StepFailed(f"the game logged a fatal line before /{pattern}/: {line[:220]}")
            if alive is not None and not alive():
                raise StepFailed(f"the game exited before /{pattern}/; {self.summary(start)}")
            if deadline is not None and time.monotonic() >= deadline:
                return None
            time.sleep(0.5)

    def check(self, start: int, alive) -> None:
        """Raises StepFailed if the game logged a fatal line after `start` or is gone."""
        self.wait(r"(?!)", 0, start, alive)

    def summary(self, start: int = 0) -> str:
        """The game's last NetGame state and last warnings/errors after `start`, for a failure report."""
        text = self.text()
        states = STATE.findall(text)
        notable = [m.group(0).strip()[:160] for m in NOTABLE.finditer(text, start)][-3:]
        return f"last state: {states[-1] if states else 'none logged'}; last warnings/errors: {notable or 'none'}"


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


def state_value(state: dict, path: str):
    value = state
    for part in str(path).split("."):
        value = value.get(part) if isinstance(value, dict) else None
    return value


def wait_nakama_log(pattern: str, since: datetime.datetime, timeout: float, alive) -> str | None:
    """The first line of the local nakama's own log since `since` matching `pattern`, polling the
    container log until it appears, the client dies, or `timeout` passes."""
    regex = re.compile(pattern)
    deadline = time.monotonic() + timeout
    while True:
        out = subprocess.run(["docker", "compose", "-f", str(NAKAMA_LOCAL / "docker-compose.yml"), "logs",
                              "--no-color", "--no-log-prefix", "--since", since.strftime("%Y-%m-%dT%H:%M:%SZ"),
                              "nakama"], capture_output=True, text=True).stdout
        for line in out.splitlines():
            if regex.search(line):
                return line
        if time.monotonic() >= deadline or (alive is not None and not alive()):
            return None
        time.sleep(1)


def state_matches(state: dict, step: dict) -> tuple[bool, str]:
    spec = step["state_until"]
    if "party_joinable" in spec:
        party = state.get("party", {})
        ok = bool(party.get("joinable")) == bool(spec["party_joinable"])
        return ok, f"party id={party.get('id')} joinable={party.get('joinable')} members={party.get('members')}"
    if "path" in spec:
        value = state
        for part in str(spec["path"]).split("."):
            if isinstance(value, list) and part.isdigit():  # a list index, e.g. recently_met.users.0.name
                value = value[int(part)] if int(part) < len(value) else None
            else:
                value = value.get(part) if isinstance(value, dict) else None
        if ("contains" in spec or "lacks" in spec) and isinstance(value, str):
            want_in, want_out = spec.get("contains"), spec.get("lacks")
            ok = (want_in is None or str(want_in) in value) and (want_out is None or str(want_out) not in value)
            return ok, f"{spec['path']}={value!r} (want text containing {want_in!r}, lacking {want_out!r})"
        if "contains" in spec or "lacks" in spec:
            items = [str(v) for v in value] if isinstance(value, list) else None
            want_in, want_out = spec.get("contains"), spec.get("lacks")
            ok = items is not None and (want_in is None or str(want_in) in items) and (want_out is None or str(want_out) not in items)
            return ok, f"{spec['path']}={value!r} (want contains {want_in!r}, lacks {want_out!r})"
        if "bits_set" in spec or "bits_clear" in spec:
            mask_set, mask_clear = int(spec.get("bits_set", 0)), int(spec.get("bits_clear", 0))
            ok = isinstance(value, int) and (value & mask_set) == mask_set and (value & mask_clear) == 0
            return ok, f"{spec['path']}={value!r} (want bits set {mask_set:#x}, clear {mask_clear:#x})"
        want = spec.get("equals")
        ok = value is not None and (value == want or (isinstance(want, str) and str(value) == want))
        return ok, f"{spec['path']}={value!r} (want {want!r})"
    if "party_members" in spec:
        party = state.get("party", {})
        members = party.get("members", [])
        ok = len(members) == int(spec["party_members"])
        if ok and "party_id" in spec:
            ok = int(party.get("id", 0)) == int(spec["party_id"])
        return ok, f"party id={party.get('id')} members={members}"
    if "party_joining" in spec:
        party = state.get("party", {})
        ok = bool(party.get("joining")) == bool(spec["party_joining"])
        if ok and "party_room" in spec:
            ok = int(party.get("room", 0)) == int(spec["party_room"])
        return ok, f"party id={party.get('id')} room={party.get('room')} joining={party.get('joining')}"
    if "friend" in spec:
        want = spec["friend"]
        target = int(want["id"])
        row = next((f for f in state.get("friends", []) if int(f["id"]) == target), None)
        if row is None:
            return False, f"friend {target} is not in the roster ({len(state.get('friends', []))} friends)"
        ok = all(str(row.get(k)) == str(v) for k, v in want.items() if k != "id")
        return ok, f"friend {target} " + " ".join(f"{k}={row.get(k)!r}" for k in ("online", "text", "party"))
    if "friend_invitable" in spec:
        target = int(spec["friend_invitable"])
        row = next((f for f in state.get("friends", []) if int(f["id"]) == target), None)
        if row is None:
            return False, f"friend {target} is not in the roster ({len(state.get('friends', []))} friends)"
        return row["invitable"] == 1, f"friend {target} online={row['online']} invitable={row['invitable']}"
    if "invite_count" in spec:
        invites = state.get("invites", [])
        want = int(spec["invite_count"])
        sender = spec.get("invite_sender")
        ok = len(invites) == want and (sender is None or (invites and int(invites[0]["sender"]) == int(sender)))
        return ok, f"invites={invites}"
    raise StepFailed(f"state_until has no known check: {sorted(spec)}")


# The client needs this much free GPU memory to reach the lobby. Measured on this host (8 GiB card):
# runs started with up to ~4 GiB already in use passed; runs started with 5.6-5.9 GiB in use died on
# "DirectX error: E_OUTOFMEMORY" before the lobby (2026-10-01, another session's GPU jobs).
GPU_FREE_NEEDED_MIB = 4096
GPU_POLL_SECONDS = 10


def gpu_free_mib() -> tuple[int | None, str]:
    """Free GPU memory in MiB and who holds the rest, from nvidia-smi; (None, why) if it can't say."""
    try:
        free = subprocess.run(["nvidia-smi", "--query-gpu=memory.free", "--format=csv,noheader,nounits"],
                              capture_output=True, text=True, check=True).stdout.split()
        apps = subprocess.run(["nvidia-smi", "--query-compute-apps=pid,used_memory", "--format=csv,noheader"],
                              capture_output=True, text=True, check=True).stdout.strip().replace("\n", "; ")
    except (OSError, subprocess.CalledProcessError) as exc:
        return None, f"nvidia-smi failed: {exc}"
    return int(free[0]), apps or "no compute processes"


def wait_for_gpu_memory(needed: int = GPU_FREE_NEEDED_MIB) -> None:
    """Wait, with no time limit, until the GPU has room for the client; log every change in holders."""
    last = None
    while True:
        free, holders = gpu_free_mib()
        if free is None:
            print(f"scenario: cannot read GPU memory ({holders}); launching anyway", flush=True)
            return
        if free >= needed:
            if last is not None:
                print(f"scenario: GPU has {free} MiB free; launching", flush=True)
            return
        pids = tuple(sorted(part.split(",")[0].strip() for part in holders.split(";")))
        if pids != last:  # log when who holds it changes, not on every byte
            print(f"scenario: waiting for GPU memory: {free} MiB free, need {needed}; held by {holders}", flush=True)
            last = pids
        time.sleep(GPU_POLL_SECONDS)


NAKAMA_LOCAL = REPO / "tools/nakama-local"
LOCAL_SERVER_CONFIG = SCRATCH.parent / "local-server"


def client_discord_id() -> str | None:
    """The Discord id this machine's client logs in with, from the newest run whose log has the
    runtime's own "login injected xpid=OVR-ORG-<id>" line (the cached token decides it, N20)."""
    for log in sorted(SCRATCH.glob("**/game.jsonl"), key=lambda f: f.stat().st_mtime, reverse=True)[:40]:
        m = re.search(r"login injected xpid=OVR-ORG-(\d+)", log.read_text(errors="replace"))
        if m:
            return m.group(1)
    return None


def write_local_server_config(peers: int = 0, friends: list | None = None, met: list | None = None) -> pathlib.Path:
    """The game config for a `server: local` scenario: an empty game JSON and, beside it, the
    runtime's config.yaml pointing at the local nakama as the seeded test account (the same template
    as tools/winvm/systest.py). Fails loudly when the local nakama is not up or not seeded."""
    state = NAKAMA_LOCAL / ".state/nakama.yml"
    if not state.exists():
        raise StepFailed("no local nakama state; run `just nakama-dev-up && just nakama-seed`")
    key = re.search(r"server_key:\s*(\S+)", state.read_text())
    if not key:
        raise StepFailed(f"no server_key in {state}")
    try:
        socket.create_connection(("127.0.0.1", 7350), timeout=5).close()
    except OSError as exc:
        raise StepFailed(f"local nakama is not listening on 127.0.0.1:7350 ({exc}); run `just nakama-dev-up`")
    sys.path.insert(0, str(NAKAMA_LOCAL))
    import seed  # noqa: E402  (constants only)
    discord_id = client_discord_id() or seed.DISCORD_ID
    # Peers and friendships as the scenario declares them ("client" = this machine's account, N = peer
    # N), on top of none: every run starts from the same friend graph.
    def account(who: str) -> str:
        return discord_id if who == "client" else str(seed.peer(int(who))[1])
    command = [sys.executable, str(NAKAMA_LOCAL / "seed.py"), "--discord-id", discord_id,
               "--peers", str(peers), "--reset-friends", "--reset-met"]
    for a, b in friends or []:
        command += ["--friends", f"{account(a)},{account(b)}"]
    for a, b in met or []:  # [owner, met]: who is on whose recently-met list
        command += ["--met", f"{account(a)},{account(b)}"]
    done = subprocess.run(command, capture_output=True, text=True)
    if done.returncode != 0:
        raise StepFailed(f"seeding the local nakama failed: {done.stderr.strip() or done.stdout.strip()}")
    print(f"scenario: local nakama account carries discord id {discord_id}", flush=True)
    LOCAL_SERVER_CONFIG.mkdir(parents=True, exist_ok=True)
    (LOCAL_SERVER_CONFIG / "config.json").write_text("{}\n")
    (LOCAL_SERVER_CONFIG / "config.yaml").write_text(
        "services:\n"
        f'  socket_uri: "ws://127.0.0.1:7350/ws?format=evr&token={key.group(1)}"\n'
        "identity:\n"
        f'  discord_id: "{discord_id}"\n'
        "auth:\n"
        f'  password: "{seed.PASSWORD}"\n'
        f'  server_key: "{key.group(1)}"\n')
    return LOCAL_SERVER_CONFIG / "config.json"


def wait_for_wineserver_exit() -> None:
    """Block until the prefix's wineserver has exited (`wineserver -w`). `-k` only signals it; a
    client launched while the old server is still going down started with a broken socket layer
    (2026-10-01, suite 20261001T212821 party_join_errors: proxy bind failed, curl init failed and the
    login connection was refused, 2.8 s after the previous game's last line)."""
    env = dict(os.environ, WINEPREFIX=str(WINEPREFIX))
    started = time.monotonic()
    subprocess.run(["wineserver", "-w"], env=env, capture_output=True)
    waited = time.monotonic() - started
    if waited >= 1:
        print(f"scenario: waited {waited:.1f} s for the previous wineserver to exit", flush=True)


class Run:
    def __init__(self, scenario: dict, dll: pathlib.Path, out: pathlib.Path):
        self.scenario, self.dll, self.out = scenario, dll, out
        self.results: list[dict] = []
        self.console: ConsoleLog | None = None
        self.control: Control | None = None
        self.mark = 0  # console offset at the last action; expect_log looks after it
        self.saved: dict = {}  # fire reply fields saved by a step's `save`, for later ${name}
        self.peers: dict = {}  # local peer accounts by number (tools/nakama-local/evr_peer.py)
        self.started_utc = datetime.datetime.now(datetime.timezone.utc)
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
        command = [str(REPO / "launch-client.sh"), "--dll", str(self.dll)]
        if self.scenario.get("server") == "local":
            command += ["--config", str(write_local_server_config(self.scenario.get("peers", 0),
                                                                  self.scenario.get("friends", []),
                                                                  self.scenario.get("met", [])))]
        self.launcher = subprocess.Popen(
            command, cwd=REPO,
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
        for peer in self.peers.values():
            peer.close()
        if self.control:
            self.control.close()
        env = dict(os.environ, WINEPREFIX=str(WINEPREFIX))
        subprocess.run(["wineserver", "-k"], env=env, capture_output=True)
        wait_for_wineserver_exit()
        if self.launcher:
            try:
                self.launcher.wait(timeout=90)
            except subprocess.TimeoutExpired:
                os.killpg(self.launcher.pid, signal.SIGTERM)
                self.launcher.wait(timeout=30)
        if self.xephyr:
            self.xephyr.terminate()
            self.xephyr.wait(timeout=15)

    def alive(self) -> bool:
        """The game is still running: launch-client.sh returns only after echovr.exe exits."""
        return self.launcher is not None and self.launcher.poll() is None

    # -- steps -------------------------------------------------------------------------------------
    def connect(self):
        if self.control:
            return
        m = re.search(r"\[NEVR\.SCENARIO\] control listening on 127\.0\.0\.1:(\d+)", self.console.text())
        if not m:
            raise StepFailed("the control endpoint never logged its port (is this the mingw-scenario DLL?)")
        self.control = Control(int(m.group(1)))

    def do(self, step: dict) -> str:
        step = substitute(step, self.saved)
        kind = step["kind"]
        if kind == "wait_log":
            spec = step["wait_log"]
            timeout = spec.get("timeout")
            m = self.console.wait(spec["pattern"], None if timeout is None else float(timeout), alive=self.alive)
            if not m:
                raise StepFailed(f"no line matching /{spec['pattern']}/ within {timeout} s; {self.console.summary()}")
            return m.group(0)[:160]
        if kind == "expect_log":
            spec = step["expect_log"]
            m = self.console.wait(spec["pattern"], float(spec.get("timeout", 10)), self.mark, alive=self.alive)
            if not m:
                found = []
                text = self.console.text()[self.mark:]
                for pattern in step.get("diagnose", []):
                    found += [line.strip()[:200] for line in text.splitlines() if re.search(pattern, line)]
                why = f"no line matching /{spec['pattern']}/ within {spec.get('timeout', 10)} s after the last action"
                if found:
                    why += "; instead: " + " | ".join(found[:4])
                why += "; " + self.console.summary(self.mark)
                raise StepFailed(why)
            return m.group(0)[:160]
        self.connect()
        if kind == "state_until":
            deadline = time.monotonic() + float(step["state_until"].get("timeout", 15))
            detail = ""
            while time.monotonic() < deadline:
                self.console.check(self.mark, self.alive)
                state = self.control.call({"op": "state"})
                if not state.get("ok"):
                    raise StepFailed(f"state failed: {state.get('error')}")
                ok, detail = state_matches(state, step)
                if ok:
                    for name, path in (step["state_until"].get("save") or {}).items():
                        self.saved[name] = state_value(state, path)
                    return detail
                time.sleep(1)
            raise StepFailed(f"condition not met: {detail}; {self.console.summary(self.mark)}")
        if kind == "peer":
            return self.do_peer(step["peer"])
        if kind == "nakama_log":
            spec = step["nakama_log"]
            if self.scenario.get("server") != "local":
                raise StepFailed("nakama_log reads the local nakama's log: the scenario needs `server: local`")
            line = wait_nakama_log(spec["pattern"], self.started_utc, float(spec.get("timeout", 15)), self.alive)
            if line is None:
                raise StepFailed(f"the local nakama logged no line matching /{spec['pattern']}/ "
                                 f"within {spec.get('timeout', 15)} s since this run started")
            return line[:200]
        command = {"op": kind, **step[kind]}
        self.mark = len(self.console.text())
        reply = self.control.call(command)
        if not reply.get("ok"):
            raise StepFailed(f"{kind} failed: {reply.get('error')}")
        for name, field in (step.get("save") or {}).items():  # reply fields later steps can use as ${name}
            if field not in reply:
                raise StepFailed(f"{kind} reply has no {field!r} to save as {name}: {json.dumps(reply)[:160]}")
            self.saved[name] = reply[field]
        return json.dumps(reply)[:160]

    def do_peer(self, spec: dict) -> str:
        """A local peer account acts: login (implicit, as `headset` if given), create_party, join,
        set_policy, lock, invite, share_member/share_party (arg: JSON text), wait_data (arg: text the
        received PartyDataNotify's JSON contains). `arg` "client" is this machine's account id; `save`
        keeps the result as ${name}."""
        if self.scenario.get("server") != "local":
            raise StepFailed("peer steps need `server: local`")
        sys.path.insert(0, str(NAKAMA_LOCAL))
        import evr_peer  # noqa: E402
        number = int(spec["id"])
        log_path = self.out / "peers.log"

        def log(line: str) -> None:
            with log_path.open("a") as f:
                f.write(f"{datetime.datetime.now().isoformat(timespec='milliseconds')} {line}\n")
        peer = self.peers.get(number)
        if peer is None:
            name, account, password = evr_peer.peer_account(number)
            peer = evr_peer.Peer(name, account, password, evr_peer.server_key(), log=log,
                                 headset=spec.get("headset", "No VR"))
            peer.connect()
            self.peers[number] = peer
        action = spec["do"]
        arg = spec.get("arg")
        if arg == "client":
            arg = client_discord_id()
        try:
            if action == "create_party":
                result = peer.create_party()
            elif action == "join":
                result = peer.join(int(arg))
            elif action == "set_policy":
                result = peer.set_policy(int(arg))
            elif action == "lock":
                result = peer.lock_party()
            elif action == "invite":
                result = peer.invite(int(arg))
            elif action == "share_member":
                result = peer.share_member(str(arg))
            elif action == "share_party":
                result = peer.share_party(str(arg))
            elif action == "wait_data":
                result = peer.wait_data(str(arg))
            elif action == "login":
                result = "logged in"
            else:
                raise StepFailed(f"unknown peer action {action!r}")
        except TimeoutError as exc:
            raise StepFailed(str(exc))
        if spec.get("expect") is not None and str(result) != str(spec["expect"]):
            raise StepFailed(f"peer {number} {action}: got {result!r}, want {spec['expect']!r}")
        for name in (spec.get("save") or {}).keys():
            self.saved[name] = result
        return f"peer {number} {action}: {result}"

    def execute(self) -> bool:
        started = time.monotonic()
        try:
            self.ensure_xephyr()
            wait_for_gpu_memory()
            wait_for_wineserver_exit()
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
