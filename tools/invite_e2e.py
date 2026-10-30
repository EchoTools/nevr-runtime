#!/usr/bin/env python3
"""The PC invite flow end to end, against production, in one command (`just invite-e2e`, Refs #1598).

Two clients (A and B), each a copy of the game install, each signed in from a stored sign-in (launch-client.sh --creds),
run on the nested display :101 under the mingw-scenario build. A creates a private game and gets an invite code; B resolves
the code and joins; A joins the same game through the same code. The verdict is read from each client's own console log:
one line per step (PASS/FAIL) with the log line that proves it, and every URL a client touched that names readyatdawn.com
(the game hardcodes those hosts; the runtime redirects them: a URL on the invite path that is not redirected is a bug).

What is driven: the runtime's invite service through the scenario control endpoint (`fire invite_create`, `fire
invite_resolve`), which builds the plan exactly as the wizard hook does (src/runtime/scenario/scenario_control.cpp). The
wizard's own cards and typed code are not clicked: no input reaches the game's menu in the nested display's setup that
this tool uses. The verdict says so.

Exit 0 only when every step passed. Exit 1: a step failed. Exit 2: the run could not start (build, display, accounts, disk).
"""
from __future__ import annotations

import argparse
import dataclasses
import datetime
import fcntl
import getpass
import hashlib
import os
import pathlib
import re
import shutil
import signal
import subprocess
import sys
import time

REPO = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPO / "tools"))
sys.path.insert(0, str(REPO / "tools/scenario"))
import work_root  # noqa: E402

DEFAULT_DLL = REPO / "build/mingw-scenario/bin/BugSplat64.dll"
RUN_ROOT = work_root.work_root() / "invite-e2e"
RUN_ROOT_PURPOSE = ("Runs of tools/invite_e2e.py (just invite-e2e): one directory per run with the two game copies "
                    "(removed when the run ends) and an evidence folder with each client's console and game logs.")
RUN_ROOT_REMOVED = "a run's copies: by the run, at its end. A run's evidence: when the PR that cites it merges."

ANSI = re.compile(r"\x1b\[[0-9;]*m")
LINE_TS = re.compile(r"^(?P<ts>\d{4}-\d\d-\d\dT[\d:.]+Z)\s+(?P<rest>.*)$")
GUID = r"[0-9A-Fa-f]{8}-[0-9A-Fa-f]{4}-[0-9A-Fa-f]{4}-[0-9A-Fa-f]{4}-[0-9A-Fa-f]{12}"

# Lines in a client's own log that mean the run is over (the scenario runner's list, reused so the two agree).
try:
    from run_scenario import FATAL, Control, StepFailed  # noqa: E402
except ImportError as exc:  # pragma: no cover - the tool needs PyYAML through run_scenario; say so rather than fall back
    raise SystemExit(f"invite_e2e needs tools/scenario/run_scenario.py and its imports: {exc}")


# ---- reading a client's log -------------------------------------------------------------------------------------------

@dataclasses.dataclass(frozen=True)
class Line:
    n: int          # 1-based line number in the console log, as the kept copy numbers it
    ts: str | None  # the line's UTC stamp, None when the line carries none
    text: str       # the line without ANSI escapes and without its stamp

    def brief(self, width: int = 150) -> str:
        return (f"{self.ts} " if self.ts else "") + self.text[:width]


def parse_lines(raw: str) -> list[Line]:
    lines = []
    rows = ANSI.sub("", raw).split("\n")
    if rows and rows[-1] == "":
        rows.pop()  # the text after the last newline is no line: the next line written takes that number (a mark is len(lines))
    for n, row in enumerate(rows, 1):
        match = LINE_TS.match(row)
        lines.append(Line(n, match["ts"], match["rest"]) if match else Line(n, None, row))
    return lines


def first(lines: list[Line], pattern: str, after: int = 0) -> tuple[Line, re.Match] | None:
    """The first line after line number `after` that matches `pattern`, with its match."""
    regex = re.compile(pattern)
    for line in lines:
        if line.n > after:
            match = regex.search(line.text)
            if match:
                return line, match
    return None


def first_bad_line(lines: list[Line], after: int = 0) -> Line | None:
    """The first line after `after` that says something went wrong: a fatal line of the harness's list, a refused invite
    answer, or a warning or error from the invite, bridge or fatal paths."""
    bad = re.compile(r"\[NEVR\.INVITE\].*(?:result=(?!ok)\w+|reason=\w+|Warning| warn )|"
                     r"\[NEVR\.(?:INVITE|WS|FATAL)\].*\b(?:warn|error)\b|\[NEVR\.FATAL\]|"
                     r"lobby session failure")
    for line in lines:
        if line.n > after and (FATAL.search(line.text) or bad.search(line.text)):
            return line
    return None


# ---- the readyatdawn.com URLs ------------------------------------------------------------------------------------------

RAD_HOST = re.compile(r"(?i)\b(?:[a-z0-9-]+\.)*readyatdawn\.com\b")
RAD_URL = re.compile(r"(?i)[a-z][a-z0-9+.-]*://[^\s\"'<>,;)]*readyatdawn\.com[^\s\"'<>,;)]*")
SERVICE_REDIRECT = re.compile(r"\[NEVR\.PATCH\] service redirect key=(?P<key>\S+) from=(?P<src>\S+) to=(?P<dst>\S+)")
HTTP_REDIRECT = re.compile(r"\[NEVR\.PATCH\] HTTP\(S\) connection redirected: (?P<src>\S+) → (?P<dst>\S+)")

# Where each kind of line is written, so a URL that slips through is named with its call site.
CALL_SITES = {
    "service_redirect": "src/runtime/lifecycle/config.cpp:629 (service redirect, the config/login/transaction sockets)",
    "http_redirect": "src/runtime/lifecycle/config.cpp:548,564 (the game's HttpConnect hook; NoteConnect at :571)",
    "other": "no redirect line: the log names the host and nothing records a redirect",
}


@dataclasses.dataclass(frozen=True)
class RadHit:
    n: int
    ts: str | None
    kind: str          # service_redirect | http_redirect | other
    url: str           # the game's URL (or the host a line names)
    final: str         # where the runtime sent it, "" when no redirect line
    redirected: bool   # the final destination no longer names readyatdawn.com

    @property
    def call_site(self) -> str:
        return CALL_SITES[self.kind]


def find_rad_urls(lines: list[Line]) -> list[RadHit]:
    """Every line that names readyatdawn.com, classified. A redirect line whose destination still names readyatdawn.com, and
    a line that names the host with no redirect, are not redirected."""
    hits: list[RadHit] = []
    for line in lines:
        if not RAD_HOST.search(line.text):
            continue
        service = SERVICE_REDIRECT.search(line.text)
        http = None if service else HTTP_REDIRECT.search(line.text)
        if service or http:
            match = service or http
            kind = "service_redirect" if service else "http_redirect"
            hits.append(RadHit(line.n, line.ts, kind, match["src"], match["dst"], not RAD_HOST.search(match["dst"])))
            continue
        urls = RAD_URL.findall(line.text) or [m.group(0) for m in RAD_HOST.finditer(line.text)]
        for url in urls:
            hits.append(RadHit(line.n, line.ts, "other", url, "", False))
    return hits


# ---- the steps and the verdict -----------------------------------------------------------------------------------------

@dataclasses.dataclass
class Step:
    name: str
    ok: bool
    proof: list[tuple[str, Line | None, str]] = dataclasses.field(default_factory=list)  # (client, line, note)
    gating: bool = True


def render(steps: list[Step], rad: dict[str, list[RadHit]], window_start: dict[str, str | None], header: list[str]) -> str:
    """The one-screen verdict. `window_start` is each client's stamp of its first invite request: a readyatdawn.com URL that is
    not redirected counts against the run only from there on (the invite path), earlier ones are listed."""
    out = list(header)
    for i, step in enumerate(steps, 1):
        flag = "PASS" if step.ok else ("FAIL" if step.gating else "info")
        out.append(f"{i:2d}. {flag}  {step.name}")
        for client, line, note in step.proof:
            where = f"{client}:console.log:{line.n}: {line.brief()}" if line else f"{client}: {note}"
            out.append(f"        {where}" + (f"  [{note}]" if line and note else ""))
    out.append("readyatdawn.com URLs the clients touched (A, B):")
    total = 0
    used: set[str] = set()
    for client in sorted(rad):
        seen = set()
        for hit in rad[client]:
            key = (hit.kind, hit.url, hit.final)
            if key in seen:
                continue
            seen.add(key)
            total += 1
            on_path = bool(hit.ts and window_start.get(client) and hit.ts >= window_start[client])
            state = "redirected" if hit.redirected else ("NOT REDIRECTED" + (" (invite path)" if on_path else ""))
            used.add(hit.kind)
            out.append(f"  {client} {state}: {hit.url}" + (f" -> {hit.final}" if hit.final else "") +
                       f"   [{client}:console.log:{hit.n}; {hit.kind}]")
    if total == 0:
        out.append("  none named in either client's log")
    for kind in sorted(used):
        out.append(f"  call site of {kind}: {CALL_SITES[kind]}")
    ok = all(s.ok for s in steps if s.gating)
    out.append(f"VERDICT: {'PASS' if ok else 'FAIL'}")
    return "\n".join(out)


def slips_on_invite_path(rad: dict[str, list[RadHit]], window_start: dict[str, str | None]) -> list[tuple[str, RadHit]]:
    bad = []
    for client, hits in rad.items():
        start = window_start.get(client)
        for hit in hits:
            if not hit.redirected and start and hit.ts and hit.ts >= start:
                bad.append((client, hit))
    return bad


# ---- accounts ---------------------------------------------------------------------------------------------------------

def stored_accounts(creds_root: pathlib.Path) -> list[str]:
    """The stored sign-ins a run may use: every directory under the store with a .credentials.json, less the ones the store's
    README marks `Do NOT use` (an account that is a person's own)."""
    readme = creds_root / "README.md"
    barred = set()
    if readme.is_file():
        for row in readme.read_text().splitlines():
            if "Do NOT use" in row:
                barred.update(re.findall(r"\b(\d{6,})/", row))
    return sorted(d.name for d in creds_root.iterdir()
                  if d.is_dir() and (d / ".credentials.json").is_file() and d.name not in barred)


# ---- the game copies --------------------------------------------------------------------------------------------------

def make_safe(copy: pathlib.Path, src: pathlib.Path) -> list[str]:
    """AGENTS.md "The game install", "Make it safe": a copy starts with no config, no plugin and no sign-in, and keeps no
    link into the install it came from. Returns what it removed, one line each."""
    done = []
    echovr = copy / "echovr"
    for name in ("config.yaml", "config.json"):
        target = echovr / "_local" / name
        if target.exists() or target.is_symlink():
            target.unlink()
            done.append(f"removed {target.relative_to(copy)}")
    for folder in [echovr / "bin/win10/plugins", *echovr.glob(".wineprefix/drive_c/users/*/AppData/Local/nevr/echovr/plugins")]:
        if folder.is_dir():
            for item in folder.iterdir():
                shutil.rmtree(item) if item.is_dir() and not item.is_symlink() else item.unlink()
                done.append(f"emptied {folder.relative_to(copy)}")
    for path in list(echovr.rglob(".credentials.json*")):
        if not path.is_dir() or path.is_symlink():
            path.unlink()
            done.append(f"removed {path.relative_to(copy)}")
    src_real = src.resolve()
    for path in list(echovr.rglob("*")):
        if path.is_symlink():
            target = pathlib.Path(os.path.realpath(path))
            if target == src_real or src_real in target.parents or pathlib.Path("/mnt/games") in target.parents:
                path.unlink()
                done.append(f"removed link {path.relative_to(copy)} -> {target}")
    return done


def start_copy(src: pathlib.Path, copy: pathlib.Path) -> subprocess.Popen:
    (copy / "echovr").mkdir(parents=True)
    return subprocess.Popen(["cp", "-a", "--reflink=auto", f"{src}/.", str(copy / "echovr")])


def tree_bytes(path: pathlib.Path) -> int:
    out = subprocess.run(["du", "-s", "--block-size=1", str(path)], capture_output=True, text=True, check=True)
    return int(out.stdout.split()[0])


# ---- a client ---------------------------------------------------------------------------------------------------------

class Client:
    def __init__(self, label: str, account: str, copy: pathlib.Path, run: pathlib.Path):
        self.label, self.account, self.copy, self.run = label, account, copy, run
        self.proc: subprocess.Popen | None = None
        self.out = run / f"launch-{label}.out"
        self.console: pathlib.Path | None = None
        self.control: Control | None = None
        self.answers: list[dict] = []
        self.started = 0.0

    @property
    def prefix(self) -> pathlib.Path:
        return self.copy / "echovr/.wineprefix"

    @property
    def logs(self) -> pathlib.Path:
        return self.prefix / "drive_c/users" / getpass.getuser() / "AppData/Local/nevr/echovr/logs"

    def launch(self, dll: pathlib.Path, timeout: int) -> None:
        env = dict(os.environ, NEVR_GAME_ROOT=str(self.copy), NEVR_SCOPE_RUN_TO_COPY="1",
                   NEVR_LAUNCH_LOCK=str(self.run / f"launch-{self.label}.lock"),
                   NEVR_RUN_SCRATCH_ROOT=str(self.run / f"scratch-{self.label}"))
        # The client in its own live-run scope (AGENTS.md "Builds"): a build elsewhere never starves it. launch-client.sh
        # wraps the game in `timeout -s INT -k 15 <timeout>`, so a login that loops ends.
        cmd = ["systemd-run", "--user", "--scope", "--quiet", "--slice=nevr-live.slice", "-p", "MemoryMax=6G",
               "-p", "MemorySwapMax=0", f"--unit=invite-e2e-{self.label}-{os.getpid()}",
               str(REPO / "launch-client.sh"), "--dll", str(dll), "--creds", self.account, "--timeout", str(timeout)]
        self.started = time.monotonic()
        self.proc = subprocess.Popen(cmd, cwd=REPO, env=env, stdout=self.out.open("w"), stderr=subprocess.STDOUT,
                                     start_new_session=True)

    def alive(self) -> bool:
        return self.proc is not None and self.proc.poll() is None

    def find_console(self, wait: float = 60.0) -> None:
        deadline = time.monotonic() + wait
        while time.monotonic() < deadline:
            match = re.search(r"=== Console log: (\S+) ===", self.out.read_text(errors="replace"))
            if match:
                self.console = pathlib.Path(match.group(1))
                return
            if not self.alive():
                raise StepFailed(f"client {self.label}: launch-client.sh exited early (rc={self.proc.returncode}); see {self.out}")
            time.sleep(0.3)
        raise StepFailed(f"client {self.label}: launch-client.sh never printed its console log path; see {self.out}")

    def lines(self) -> list[Line]:
        try:
            return parse_lines(self.console.read_text(errors="replace")) if self.console else []
        except FileNotFoundError:
            return []

    def wait_for(self, pattern: str, timeout: float, after: int = 0) -> tuple[Line, re.Match]:
        """Block until a line after `after` matches; StepFailed (naming the first bad line) when the client logs a fatal line,
        exits, or the time runs out."""
        deadline = time.monotonic() + timeout
        while True:
            lines = self.lines()
            hit = first(lines, pattern, after)
            if hit:
                return hit
            bad = first_bad_line(lines, after)
            if bad and FATAL.search(bad.text):
                raise StepFailed(f"client {self.label} console.log:{bad.n}: {bad.brief(200)}")
            if not self.alive():
                raise StepFailed(f"client {self.label} exited before /{pattern}/ (launcher rc={self.proc.returncode})")
            if time.monotonic() >= deadline:
                bad = first_bad_line(lines, after)
                raise StepFailed(f"client {self.label}: no line /{pattern}/ within {timeout:.0f} s" +
                                 (f"; first bad line console.log:{bad.n}: {bad.brief(200)}" if bad else ""))
            time.sleep(0.5)

    def connect(self) -> None:
        if self.control:
            return
        _, match = self.wait_for(r"\[NEVR\.SCENARIO\] control listening on 127\.0\.0\.1:(\d+)", 120)
        self.control = Control(int(match.group(1)))

    def call(self, command: dict) -> dict:
        reply = self.control.call(command)
        if not reply.get("ok"):
            raise StepFailed(f"client {self.label}: {command.get('op')} {command.get('action', '')} failed: {reply.get('error')}")
        return reply

    def answer(self, op: str, timeout: float) -> dict:
        """The next answer of `op` the invite applier took (the control endpoint hands each over once)."""
        deadline = time.monotonic() + timeout
        while True:
            self.answers += self.call({"op": "invite_answers"})["answers"]
            for row in self.answers:
                if row["op"] == op:
                    self.answers.remove(row)
                    return row
            if time.monotonic() >= deadline:
                raise StepFailed(f"client {self.label}: no {op} answer within {timeout:.0f} s")
            time.sleep(1)


# ---- the run ----------------------------------------------------------------------------------------------------------

def say(message: str) -> None:
    print(f"[invite-e2e {datetime.datetime.now().strftime('%H:%M:%S')}] {message}", file=sys.stderr, flush=True)


def uuid_of(match_id: str) -> str:
    """The game's id from an answer's match id (`<uuid>.<node>`), upper case as the client logs it."""
    return match_id[:36].upper()


def run_flow(a: Client, b: Client, steps: list[Step], timing: dict) -> None:
    """The steps, in order. Each appends its Step; the first StepFailed ends the flow and is recorded by the caller."""
    def done(name: str, proof, ok: bool = True, gating: bool = True) -> None:
        steps.append(Step(name, ok, proof, gating))
        say(f"{'PASS' if ok else 'FAIL'}  {name}")

    for c in (a, b):
        c.find_console()
    proofs = []
    for c in (a, b):
        line, _ = c.wait_for(r"NetGame switching state \(from logging in, to logged in\)", 300)
        proofs.append((c.label, line, ""))
    done("both clients signed in (stored sign-ins, production login)", proofs)
    for c in (a, b):
        c.connect()
    proofs = []
    for c in (a, b):
        line, _ = c.wait_for(r"\[NEVR\.INVITE\] service on", 30)
        proofs.append((c.label, line, ""))
    done("invite service on in both clients", proofs)

    # A: CREATE
    mark = a.lines()[-1].n if a.lines() else 0
    a.call({"op": "fire", "action": "invite_create", "expires": "30 MINUTES", "experience": "arena", "mode": "custom"})
    sent, _ = a.wait_for(r"\[NEVR\.INVITE\] request sent op=create", 60, mark)
    timing["window_start"][a.label] = sent.ts
    answered, am = a.wait_for(r"\[NEVR\.INVITE\] answer op=create status=(\d+) latency_ms=\d+ result=(\w+)", 90, mark)
    created = a.answer("create", 30)
    ok = am[1] == "200" and am[2] == "ok" and bool(created.get("code")) and bool(created.get("match_id"))
    done("A creates a game and gets an invite code (invite/create)",
         [("A", sent, ""), ("A", answered, f"code_len={len(created.get('code', ''))} expires={created.get('expires')!r} match={created.get('match_id')}")],
         ok)
    if not ok:
        raise StepFailed(f"A: create answered status={am[1]} result={am[2]}")
    posted = first(a.lines(), r"\[NEVR\.INVITE\] text posted field=code bytes=(\d+)", mark)
    done("the code is written into the wizard's share page (text posted field=code)",
         [("A", posted[0] if posted else None, "no `text posted field=code` line after the create")], bool(posted))
    code, match_uuid = created["code"], uuid_of(created["match_id"])

    # B: JOIN VIA CODE; A joins the same game through the same code
    joined: dict[str, str] = {}
    for c in (b, a):
        mark = c.lines()[-1].n if c.lines() else 0
        c.call({"op": "fire", "action": "invite_resolve", "code": code})
        req, _ = c.wait_for(r"\[NEVR\.INVITE\] request sent op=resolve code_len=\d+", 30, mark)
        ans, rm = c.wait_for(r"\[NEVR\.INVITE\] answer op=resolve status=(\d+) latency_ms=\d+ result=(\w+)", 60, mark)
        row = c.answer("resolve", 30)
        start, sm = c.wait_for(rf"\[NEVR\.INVITE\] join started route=(\w+) lobby=({GUID}) state_from=(\d+)", 60, mark)
        joined[c.label] = sm[2].upper()
        done(f"{c.label} resolves the code and starts the join (invite/resolve, join started)",
             [(c.label, ans, ""), (c.label, start, f"resolved match={row.get('match_id')}")],
             rm[1] == "200" and rm[2] == "ok" and sm[1] == "stage")
        timing["mark"][c.label] = mark

    # both clients: join request, session success, same lobby and group
    sessions: dict[str, tuple[str, str]] = {}
    for c in (b, a):
        mark = timing["mark"][c.label]
        req, rqm = c.wait_for(rf"\[NEVR\.WS\] lobby join requested conn=\d+ lobby=({GUID})", 240, mark)
        suc, sm = c.wait_for(rf"\[NEVR\.WS\] lobby session success conn=\d+ lobby=({GUID}) group=({GUID})", 240, mark)
        sessions[c.label] = (sm[1].upper(), sm[2].upper())
        done(f"{c.label} sends a lobby join and the game service answers session success",
             [(c.label, req, ""), (c.label, suc, "")], rqm[1].upper() == sm[1].upper())
    same = (joined["A"] == joined["B"] == sessions["A"][0] == sessions["B"][0] == match_uuid
            and sessions["A"][1] == sessions["B"][1])
    done("A and B are in the same match (create's match id = both joins = both session successes, same group)",
         [("A", None, f"match {match_uuid} group {sessions['A'][1]}"), ("B", None, f"match {sessions['B'][0]} group {sessions['B'][1]}")],
         same)

    # both reach in game
    proofs = []
    for c in (b, a):
        line, _ = c.wait_for(r"NetGame switching state \(from [^)]*, to in game\)", 420, timing["mark"][c.label])
        proofs.append((c.label, line, ""))
    done("both clients reach in game in that match", proofs)

    # soft evidence they see each other: the game asks for the other player's profile
    seen = []
    for c in (a, b):
        hit = first(c.lines(), r"SNSOtherUserProfileRequest", timing["mark"][c.label])
        seen.append((c.label, hit[0] if hit else None, "no SNSOtherUserProfileRequest yet"))
    steps.append(Step("each client asks for the other player's profile (soft: the roster needs menu input)",
                      all(line for _, line, _ in seen), seen, gating=False))


def teardown(clients: list[Client], evidence: pathlib.Path) -> list[Step]:
    """Stop each client, keep its logs, check the original DLL is back and the copy is gone."""
    results = []
    for c in clients:
        if c.control:
            c.control.close()
        env = dict(os.environ, WINEPREFIX=str(c.prefix))
        subprocess.run(["wineserver", "-k"], env=env, capture_output=True)
    for c in clients:
        env = dict(os.environ, WINEPREFIX=str(c.prefix))
        subprocess.run(["timeout", "30", "wineserver", "-w"], env=env, capture_output=True)
        if c.proc is not None:
            try:
                c.proc.wait(timeout=120)
            except subprocess.TimeoutExpired:
                os.killpg(c.proc.pid, signal.SIGTERM)
                c.proc.wait(timeout=30)
        dest = evidence / c.label
        dest.mkdir(parents=True, exist_ok=True)
        if c.console and c.console.is_file():
            shutil.copy(c.console, dest / "console.log")
        for log in sorted(c.logs.glob("nevr-*.jsonl")) if c.logs.is_dir() else []:
            shutil.copy(log, dest / log.name)
        launched = c.proc is not None and c.out.is_file()
        if launched:
            shutil.copy(c.out, dest / "launch-client.out")
        restored = launched and "original BugSplat64.dll restored and verified" in c.out.read_text(errors="replace")
        results.append(Step(f"{c.label}: launch-client.sh put the original BugSplat64.dll back and removed the runtime and the sign-in link",
                            restored, [(c.label, None, f"launch-client.sh rc={c.proc.returncode if c.proc else 'not started'}")]))
    return results


def remove_copy(copy: pathlib.Path, run: pathlib.Path) -> None:
    """Remove a game copy this run made, by its exact path, after checking the path is one of this run's own."""
    if copy.parent != run or not copy.name.startswith("copy-"):
        raise SystemExit(f"refusing to remove {copy}: not a copy this run made under {run}")
    shutil.rmtree(copy)


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--dll", type=pathlib.Path, default=DEFAULT_DLL, help="the mingw-scenario BugSplat64.dll")
    parser.add_argument("--account-a", help="stored sign-in for client A (a directory name under the creds store)")
    parser.add_argument("--account-b", help="stored sign-in for client B")
    parser.add_argument("--timeout", type=int, default=1500, help="seconds each client may run (timeout -s INT -k 15)")
    parser.add_argument("--game-root", type=pathlib.Path,
                        help="the install to copy (the directory that holds bin/win10/echovr.exe); default: the main checkout's echovr/")
    args = parser.parse_args(argv)

    # -- preconditions: each failure here is exit 2 with the fix, before any copy is made ----------------------------------
    runtime = args.dll.with_name("nevr-runtime.dll")
    if not args.dll.is_file() or not runtime.is_file() or b"[NEVR.SCENARIO]" not in runtime.read_bytes():
        print(f"ERROR: {args.dll} is not a scenario build; run `just preset=mingw-scenario build`", file=sys.stderr)
        return 2
    if subprocess.run(["pgrep", "-f", r"([X]vfb|[X]ephyr) :101"], capture_output=True).returncode != 0:
        print("ERROR: no X server on :101; start `Xvfb :101 -screen 0 1920x1080x24` (clients run on the nested display only)", file=sys.stderr)
        return 2
    creds_root = work_root.work_root() / "creds"
    usable = stored_accounts(creds_root)
    account_a = args.account_a or (usable[0] if len(usable) >= 2 else None)
    account_b = args.account_b or (usable[1] if len(usable) >= 2 else None)
    if not account_a or not account_b or account_a == account_b:
        print(f"ERROR: two different stored sign-ins are needed (found {len(usable)} usable in {creds_root}); a missing one needs "
              "`launch-client.sh --device-code --save-creds <account>` and a person to enter the code", file=sys.stderr)
        return 2
    src = args.game_root
    if src is None:
        out = subprocess.run([str(REPO / "launch-client.sh"), "--print-game-root"], capture_output=True, text=True, check=True)
        src = pathlib.Path(out.stdout.strip()) / "echovr"
    src = src.resolve()
    if not (src / "bin/win10/echovr.exe").is_file():
        print(f"ERROR: {src} holds no bin/win10/echovr.exe", file=sys.stderr)
        return 2
    need = 2 * tree_bytes(src)
    work_root.ensure_dir(RUN_ROOT, RUN_ROOT_PURPOSE, RUN_ROOT_REMOVED)
    free = shutil.disk_usage(RUN_ROOT).free
    if free < need + 4 * 2**30:
        print(f"ERROR: two copies of the install need {need / 2**30:.1f} GiB and 4 GiB spare; {free / 2**30:.1f} GiB is free under {RUN_ROOT}", file=sys.stderr)
        return 2

    # one game run at a time, whoever launches it (the lock every launcher takes), held for the whole flow
    lock_path = work_root.work_root() / "launch-client.lock"
    lock = open(lock_path, "w")
    try:
        fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
    except OSError:
        print(f"ERROR: another game run holds {lock_path}", file=sys.stderr)
        return 2
    from run_scenario import game_pids, wait_for_gpu_memory  # noqa: E402
    if game_pids():
        print(f"ERROR: echovr.exe is already running (pid {' '.join(game_pids())}); stop it first", file=sys.stderr)
        return 2

    stamp = datetime.datetime.now().strftime("%Y%m%dT%H%M%S")
    run = RUN_ROOT / stamp
    evidence = run / "evidence"
    run.mkdir(parents=True)
    clients = [Client("A", account_a, run / "copy-A", run), Client("B", account_b, run / "copy-B", run)]
    steps: list[Step] = []
    timing = {"window_start": {}, "mark": {}}
    host_sha = hashlib.sha256(args.dll.read_bytes()).hexdigest()[:12]
    runtime_sha = hashlib.sha256(runtime.read_bytes()).hexdigest()[:12]
    head = subprocess.run(["git", "rev-parse", "--short=10", "HEAD"], cwd=REPO, capture_output=True, text=True).stdout.strip()
    header = [f"invite-e2e {stamp}  HEAD {head}  BugSplat64.dll {host_sha}  nevr-runtime.dll {runtime_sha}  (production, nested display :101)",
              "driven: invite service via the scenario control endpoint (fire invite_create / invite_resolve); the wizard's cards and typed code are NOT clicked"]
    rad: dict[str, list[RadHit]] = {}
    try:
        say(f"copying the install ({need / 2 / 2**30:.1f} GiB) twice into {run}")
        copies = [start_copy(src, c.copy) for c in clients]
        for proc, c in zip(copies, clients):
            if proc.wait() != 0:
                raise StepFailed(f"cp of the install into {c.copy} failed (rc={proc.returncode})")
            removed = make_safe(c.copy, src)
            say(f"copy {c.label}: {len(removed)} item(s) made safe")
        wait_for_gpu_memory()
        for c in clients:
            c.launch(args.dll, args.timeout)
            say(f"client {c.label} launched (account {c.account[:4]}...; timeout {args.timeout} s)")
            time.sleep(4)
        run_flow(clients[0], clients[1], steps, timing)
    except (StepFailed, OSError, KeyError, IndexError, ValueError) as exc:  # a bug in this tool still ends in teardown and a verdict
        steps.append(Step("the flow stopped here", False, [("run", None, f"{type(exc).__name__}: {exc}")]))
        say(f"FAIL  {exc}")
    finally:
        steps += teardown(clients, evidence)
        for c in clients:
            raw = (evidence / c.label / "console.log")
            rad[c.label] = find_rad_urls(parse_lines(raw.read_text(errors="replace"))) if raw.is_file() else []
        for c in clients:
            try:
                if c.copy.exists():
                    remove_copy(c.copy, run)
            except OSError as exc:
                steps.append(Step(f"{c.label}: the game copy was removed", False, [(c.label, None, str(exc))]))
        lock.close()
    slips = slips_on_invite_path(rad, timing["window_start"])
    steps.append(Step("no readyatdawn.com URL slipped through on the invite path", not slips,
                      [(client, None, f"{hit.url} (console.log:{hit.n}; {hit.call_site})") for client, hit in slips]))
    verdict = render(steps, rad, timing["window_start"], header)
    (run / "verdict.txt").write_text(verdict + "\n")
    print(verdict)
    print(f"evidence: {evidence}")
    return 0 if all(s.ok for s in steps if s.gating) else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
