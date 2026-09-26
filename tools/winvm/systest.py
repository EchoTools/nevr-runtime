#!/usr/bin/env python3
"""System test: run the built NEVR runtime on a real (native) Windows guest.

Everything else in this repo is tested under Wine, which cannot reproduce
Windows-only failures (issue #13 was "works fine under Wine"). This drives a
libvirt Windows VM over WinRM + SMB and judges the result with checks.py.

Setup, scenarios and pitfalls: docs/reference/windows-vm-system-test.md

    WINVM_USER=... WINVM_PASS=... just test-winvm
    WINVM_USER=... WINVM_PASS=... tools/winvm/systest.py --scenario gai

Exit codes: 0 pass, 1 the runtime failed a check, 2 the environment is not
usable (no VM, no login, no game data, no build). Keeping 1 and 2 apart is the
point: "the VM is misconfigured" must never read as "the runtime regressed".
"""

from __future__ import annotations

import argparse
import datetime
import os
import pathlib
import re
import shutil
import subprocess
import sys
import tempfile
import time

HERE = pathlib.Path(__file__).resolve().parent
REPO = HERE.parents[1]
sys.path.insert(0, str(HERE))
import checks  # noqa: E402

ROOT = "C:\\nevr-systest"          # the rig; never the user's own install
GAME = "C:\\echovr"                # the user's install, used read-only
SMB_ROOT = "nevr-systest"

OFFLINE_CONFIG = """{
  "apiservice_host": "http://127.0.0.1:1/api",
  "configservice_host": "ws://127.0.0.1:1/spr",
  "loginservice_host": "ws://127.0.0.1:1/spr",
  "matchingservice_host": "ws://127.0.0.1:1",
  "serverdb_host": "ws://127.0.0.1:1/spr",
  "transactionservice_host": "ws://127.0.0.1:1/spr",
  "publisher_lock": "echovrce"
}
"""


NAKAMA_DIR = REPO / "tools/nakama-local"
NAKAMA_HOST = "192.168.122.1"  # where the guest reaches the local nakama (tools/nakama-local/setup.py)


def nakama_runtime_config() -> str:
    """config.yaml pointing a guest runtime at the local nakama, as the seeded test account.

    The server_key rides in socket_uri: the EVR /ws upgrade is a 401 without token=<server_key>
    (server/socket_ws.go), and production's proxy adds it, so the bridge does not.
    """
    state = NAKAMA_DIR / ".state/nakama.yml"
    if not state.exists():
        raise EnvError("no local nakama state; run `just nakama-up && just nakama-seed`")
    m = re.search(r"server_key:\s*(\S+)", state.read_text())
    if not m:
        raise EnvError(f"no server_key in {state}")
    sys.path.insert(0, str(NAKAMA_DIR))
    import seed  # noqa: E402  (constants only)
    return f"""services:
  socket_uri: "ws://{NAKAMA_HOST}:7350/ws?format=evr&token={m.group(1)}"
identity:
  discord_id: "{seed.DISCORD_ID}"
auth:
  password: "{seed.PASSWORD}"
  server_key: "{m.group(1)}"
"""


def nakama_log_since(since: str) -> str:
    p = subprocess.run(["docker", "compose", "-f", str(NAKAMA_DIR / "docker-compose.yml"),
                        "logs", "nakama", "--no-log-prefix", "--since", since],
                       capture_output=True, text=True)
    if p.returncode != 0:
        raise EnvError(f"cannot read local nakama logs: {p.stderr.strip()}")
    return p.stdout


# Login scenario: no *_host keys, so the game's readyatdawn.com defaults are what the
# runtime redirects to the bridge. (The offline config's 127.0.0.1:1 hosts override the
# redirect and the login service is then unreachable.)
LOGIN_CONFIG = """{
  "publisher_lock": "echovrce"
}
"""


class EnvError(Exception):
    """The environment cannot run the test. Exit code 2."""


class Guest:
    def __init__(self, host: str, user: str, password: str):
        self.host, self.user, self.password = host, user, password
        try:
            import winrm  # pywinrm
        except ImportError as e:
            raise EnvError("pywinrm is not installed (pip install pywinrm)") from e
        if not shutil.which("smbclient"):
            raise EnvError("smbclient is not installed")
        self._session = winrm.Session(
            f"http://{host}:5985/wsman", auth=(user, password), transport="ntlm",
            read_timeout_sec=180, operation_timeout_sec=150)

    def ps(self, script: str) -> str:
        """Run PowerShell on the guest; raise on a non-zero exit."""
        r = self._session.run_ps("$ProgressPreference='SilentlyContinue'\n" + script)
        out = r.std_out.decode("utf-8", "replace")
        if r.status_code != 0:
            err = re.sub(r"#< CLIXML.*", "", r.std_err.decode("utf-8", "replace"), flags=re.S).strip()
            raise RuntimeError(f"guest powershell rc={r.status_code}: {err or out}")
        return out

    def _smb(self, command: str) -> None:
        env = dict(os.environ, PASSWD=self.password)  # never on argv
        p = subprocess.run(["smbclient", f"//{self.host}/C$", "-U", self.user, "-c", command],
                           env=env, capture_output=True, text=True)
        if p.returncode != 0 or "NT_STATUS" in p.stdout + p.stderr:
            raise RuntimeError(f"smbclient failed: {(p.stdout + p.stderr).strip()}")

    def put(self, local: pathlib.Path, remote_dir: str, name: str) -> None:
        self._smb(f'cd "{remote_dir}"; put "{local}" "{name}"')

    def get(self, remote_dir: str, name: str, local: pathlib.Path) -> None:
        self._smb(f'cd "{remote_dir}"; get "{name}" "{local}"')


def resolve_host(domain: str) -> str:
    """IP of a libvirt guest, via the guest agent, then the ARP table."""
    for source in ("agent", "arp", "lease"):
        p = subprocess.run(["virsh", "-c", "qemu:///system", "domifaddr", domain, "--source", source],
                           capture_output=True, text=True)
        for ip in re.findall(r"ipv4\s+(\d+\.\d+\.\d+\.\d+)/", p.stdout):
            if not ip.startswith("127."):
                return ip
    raise EnvError(f"could not find an IPv4 address for libvirt domain {domain!r}; set WINVM_HOST")


# --- guest-side steps -----------------------------------------------------------

def preflight(g: Guest) -> None:
    facts = g.ps(r"""
$os = (Get-CimInstance Win32_OperatingSystem)
"os=$($os.Caption)"
"build=$($os.BuildNumber)"
"echovr=$(Test-Path '@GAME@\bin\win10\echovr.exe')"
$ok = Get-ChildItem '@GAME@\_data' -Directory -ErrorAction SilentlyContinue |
  Where-Object { Test-Path "$($_.FullName)\rad15\win10\packages\*" }
"packages=$([bool]$ok)"
"nested=$(Test-Path '@GAME@\_data\_data')"
"session=" + ((qwinsta 2>&1) -match 'console\s+\S+\s+\d+\s+Active' -as [bool])
""".replace("@GAME@", GAME))
    kv = dict(line.split("=", 1) for line in facts.strip().splitlines() if "=" in line)
    print(f"guest: {kv.get('os')} build {kv.get('build')}")
    if kv.get("echovr") != "True":
        raise EnvError(f"no game at {GAME}\\bin\\win10\\echovr.exe on the guest")
    if kv.get("packages") != "True":
        hint = (f" The packages exist under {GAME}\\_data\\_data (extracted one level too deep): "
                "add a directory junction at the expected path." if kv.get("nested") == "True" else "")
        raise EnvError(f"game data packages are missing under {GAME}\\_data\\<ver>\\rad15\\win10\\packages.{hint}")
    if not kv.get("session", "").startswith("True"):
        raise EnvError("no active interactive console session on the guest; the game needs a desktop. "
                       "Log in once at the VM console (or leave RDP/SPICE logged in).")


def setup_rig(g: Guest, tmp: pathlib.Path, with_legacy_dbgcore: bool = False, runtime_yaml: str | None = None) -> None:
    """Isolated copy of the game binaries, junctions to shared data, own _local.

    The user's install is left untouched: it may hold a legacy dbgcore.dll (NEVR
    refuses to run next to it) and a config.json with production credentials.
    """
    g.ps(r"""
$src='@GAME@'; $root='@ROOT@'
New-Item -ItemType Directory -Force -Path "$root\echovr\bin\win10","$root\echovr\_local","$root\run" | Out-Null
# /A-:R drops the ReadOnly attribute the install's files carry; left on, Remove-Item cannot clear them.
robocopy "$src\bin\win10" "$root\echovr\bin\win10" /E /A-:R /XF dbgcore.dll BugSplat64.dll BugSplat64.dll.stock /XD logs /NFL /NDL /NJH /NJS /NP | Out-Null
if ($LASTEXITCODE -ge 8) { throw "robocopy failed rc=$LASTEXITCODE" }
# robocopy without /MIR never deletes: clear anything a previous run left behind.
$dbg = "$root\echovr\bin\win10\dbgcore.dll"
if (Test-Path $dbg) { Remove-Item $dbg -Force }   # -Force: a leftover copy is ReadOnly
if (Test-Path $dbg) { throw "could not remove stale $dbg from the rig" }
if ('@LEGACY@' -eq 'yes') { Copy-Item "$src\bin\win10\dbgcore.dll" $dbg }
foreach ($d in '_data','content','sourcedb') {
  $l = "$root\echovr\$d"
  if (-not (Test-Path $l)) { New-Item -ItemType Junction -Path $l -Target "$src\$d" | Out-Null }
}
""".replace("@GAME@", GAME).replace("@ROOT@", ROOT).replace("@LEGACY@", "yes" if with_legacy_dbgcore else "no"))
    cfg = tmp / "config.json"
    cfg.write_text(OFFLINE_CONFIG if runtime_yaml is None else LOGIN_CONFIG)
    g.put(cfg, f"{SMB_ROOT}/echovr/_local", "config.json")
    g.put(HERE / "enum_windows.ps1", f"{SMB_ROOT}/run", "enum_windows.ps1")
    if runtime_yaml is not None:
        y = tmp / "config.yaml"
        y.write_text(runtime_yaml)
        g.put(y, f"{SMB_ROOT}/echovr/_local", "config.yaml")
    else:
        g.ps(rf"Remove-Item '{ROOT}\echovr\_local\config.yaml' -Force -ErrorAction SilentlyContinue")


def deploy(g: Guest, dll: pathlib.Path) -> None:
    kill_game(g)
    g.put(dll, f"{SMB_ROOT}/echovr/bin/win10", "BugSplat64.dll")


def kill_game(g: Guest) -> None:
    g.ps(r"""Get-Process echovr -ErrorAction SilentlyContinue |
  Where-Object { $_.Path -like '@ROOT@*' } | Stop-Process -Force; Start-Sleep 1""".replace("@ROOT@", ROOT))


def _interactive_task(name: str, execute: str, argument: str) -> str:
    # The game needs a desktop, which a WinRM (services-session) process does not have,
    # so it is started as an interactive scheduled task inside the console session.
    return rf"""
Unregister-ScheduledTask -TaskName {name} -Confirm:$false -ErrorAction SilentlyContinue
$a = New-ScheduledTaskAction -Execute '{execute}' -Argument '{argument}'
$p = New-ScheduledTaskPrincipal -UserId "$env:COMPUTERNAME\$env:USERNAME" -LogonType Interactive -RunLevel Limited
$s = New-ScheduledTaskSettingsSet -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries -ExecutionTimeLimit (New-TimeSpan -Hours 1)
Register-ScheduledTask -TaskName {name} -Action $a -Principal $p -Settings $s -Force | Out-Null
Start-ScheduledTask -TaskName {name}
"""


def launch(g: Guest, game_args: str) -> None:
    g.ps(rf"""
Remove-Item '{ROOT}\run\marker.txt','{ROOT}\run\stdout.txt','{ROOT}\run\windows.txt' -ErrorAction SilentlyContinue
Set-Content -Path '{ROOT}\run\run.cmd' -Encoding ASCII -Value @(
  '@echo off',
  'cd /d {ROOT}\echovr\bin\win10',
  'echo started %date% %time% > {ROOT}\run\marker.txt',
  'echovr.exe {game_args} > {ROOT}\run\stdout.txt 2>&1',
  'echo exited rc=%errorlevel% >> {ROOT}\run\marker.txt'
)
""" + _interactive_task("nevrsystest", "cmd.exe", rf"/c {ROOT}\run\run.cmd"))


def poll(g: Guest) -> dict:
    out = g.ps(rf"""
$m = Get-Content '{ROOT}\run\marker.txt' -ErrorAction SilentlyContinue
$p = Get-Process echovr -ErrorAction SilentlyContinue | Where-Object {{ $_.Path -like '{ROOT}*' }}
"alive=$([bool]$p)"
"marker=" + ($m -join ' / ')
""")
    kv = dict(line.split("=", 1) for line in out.strip().splitlines() if "=" in line)
    marker = kv.get("marker", "")
    exit_code = None
    m = re.search(r"exited rc=(-?\d+)", marker)
    if m:
        exit_code = int(m.group(1))
    return {"alive": kv.get("alive") == "True", "exit_code": exit_code, "started": "started" in marker}


def read_text(g: Guest, path: str) -> str:
    # Get-Content opens with FileShare.ReadWrite; ReadAllText cannot read a file cmd.exe is still writing.
    return g.ps(rf"if (Test-Path '{path}') {{ Get-Content -LiteralPath '{path}' -Raw }}")


def window_dump(g: Guest) -> str:
    g.ps(_interactive_task("nevrwin", "powershell.exe",
                           rf"-NoProfile -ExecutionPolicy Bypass -File {ROOT}\run\enum_windows.ps1 -OutFile {ROOT}\run\windows.txt"))
    for _ in range(20):
        time.sleep(1)
        try:
            text = read_text(g, rf"{ROOT}\run\windows.txt")
        except RuntimeError:
            continue  # the guest script still has the file open; try again
        if text.strip():
            return text
    return ""


def minidump(g: Guest, out: pathlib.Path) -> pathlib.Path | None:
    g.ps(rf"""$p = Get-Process echovr | Where-Object {{ $_.Path -like '{ROOT}*' }} | Select-Object -First 1
if ($p) {{ Remove-Item '{ROOT}\run\hang.dmp' -ErrorAction SilentlyContinue
  rundll32.exe C:\Windows\System32\comsvcs.dll, MiniDump $p.Id {ROOT}\run\hang.dmp full; Start-Sleep 10 }}""")
    dest = out / "hang.dmp"
    try:
        g.get(f"{SMB_ROOT}/run", "hang.dmp", dest)
    except RuntimeError:
        return None
    return dest


# --- scenarios --------------------------------------------------------------------

def scenario_boot(g: Guest, dll: pathlib.Path, out: pathlib.Path, args, login: bool = False) -> list[checks.Result]:
    with tempfile.TemporaryDirectory() as t:
        setup_rig(g, pathlib.Path(t), args.with_legacy_dbgcore, nakama_runtime_config() if login else None)
    deploy(g, dll)
    started = datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")
    launch(g, args.game_args)
    deadline = time.monotonic() + args.wait
    state: dict = {"alive": False, "exit_code": None, "started": False}
    print(f"launched; waiting up to {args.wait}s (game splash alone takes 15-20s)")
    while time.monotonic() < deadline:
        time.sleep(5)
        state = poll(g)
        if state["exit_code"] is not None:
            break
    log = read_text(g, rf"{ROOT}\run\stdout.txt")
    dump = window_dump(g) if state["alive"] else ""
    (out / "stdout.txt").write_text(log)
    (out / "windows.txt").write_text(dump)
    results = [
        checks.check_process_alive(state["alive"], state["exit_code"], args.expect),
        checks.check_no_modal_dialog(dump),
        checks.check_no_fatal(log, state["exit_code"]),
        *checks.check_hooks(log),
        checks.check_engine_progress(log, args.require_stage),
    ]
    if login:
        nlog = nakama_log_since(started)
        (out / "nakama.log").write_text(nlog)
        results.append(checks.check_nakama_login(nlog, seed_discord_id()))
    if args.dump_on_fail and state["alive"] and not checks.overall(results):
        path = minidump(g, out)
        print(f"minidump: {path or 'FAILED to fetch'}  (analyse with tools/winvm/dump_stacks.py)")
    return results


def seed_discord_id() -> str:
    sys.path.insert(0, str(NAKAMA_DIR))
    import seed  # noqa: E402
    return seed.DISCORD_ID


def scenario_gai(g: Guest, out: pathlib.Path) -> list[checks.Result]:
    cc = shutil.which("x86_64-w64-mingw32-gcc")
    if not cc:
        raise EnvError("x86_64-w64-mingw32-gcc not found (needed to build the getaddrinfo probe)")
    exe = out / "gai_probe.exe"
    subprocess.run([cc, "-O0", "-Wall", "-Wextra", "-Werror", "-o", str(exe),
                    str(HERE / "gai_probe.c"), "-lws2_32"], check=True)
    g.ps(rf"New-Item -ItemType Directory -Force -Path '{ROOT}\run' | Out-Null")
    g.put(exe, f"{SMB_ROOT}/run", "gai_probe.exe")
    text = g.ps(rf"& '{ROOT}\run\gai_probe.exe' 3")
    (out / "gai_probe.txt").write_text(text)
    return [checks.check_getaddrinfo(text)]


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--scenario", choices=["boot", "gai", "login", "all"], default="all")
    ap.add_argument("--domain", default=os.environ.get("WINVM_DOMAIN", "win11-dev"),
                    help="libvirt domain, used to find the IP when WINVM_HOST is unset")
    ap.add_argument("--dll", type=pathlib.Path, default=REPO / "build/mingw-release/bin/BugSplat64.dll")
    ap.add_argument("--game-args", default="-noovr -server -headless -noconsole",
                    help="note: -noconsole is rejected by the game unless -headless is also given")
    ap.add_argument("--wait", type=int, default=90, help="seconds to observe the boot (minimum 45)")
    ap.add_argument("--expect", choices=["alive", "exit"], default="alive")
    ap.add_argument("--require-stage", default="broadcaster",
                    choices=[n for n, _ in checks.ENGINE_STAGES])
    ap.add_argument("--with-legacy-dbgcore", action="store_true",
                    help="put the guest install's legacy Echo Relay dbgcore.dll in the rig "
                         "(NEVR refuses to run beside it unless -allow-dbgcore is in --game-args)")
    ap.add_argument("--dump-on-fail", action="store_true", help="fetch a minidump if the game is stuck")
    ap.add_argument("--out", type=pathlib.Path)
    args = ap.parse_args()
    args.wait = max(args.wait, 45)

    out = args.out or pathlib.Path("/var/tmp/work-nevr-runtime") / (
        "winvm-" + datetime.datetime.now().strftime("%Y%m%d-%H%M%S"))
    out.mkdir(parents=True, exist_ok=True)

    results: list[checks.Result] = []
    guest = None
    try:
        user, password = os.environ.get("WINVM_USER"), os.environ.get("WINVM_PASS")
        if not user or not password:
            raise EnvError("set WINVM_USER and WINVM_PASS (never commit them)")
        if args.scenario in ("boot", "login", "all") and not args.dll.exists():
            raise EnvError(f"{args.dll} not found; run `just build` first")
        host = os.environ.get("WINVM_HOST") or resolve_host(args.domain)
        print(f"guest {host} as {user}; artifacts in {out}")
        guest = Guest(host, user, password)
        preflight(guest)
        if args.scenario in ("gai", "all"):
            results += scenario_gai(guest, out)
        if args.scenario in ("boot", "all"):
            print(f"runtime under test: {args.dll} ({args.dll.stat().st_size} bytes)")
            results += scenario_boot(guest, args.dll, out, args)
        if args.scenario == "login":
            print(f"runtime under test: {args.dll} ({args.dll.stat().st_size} bytes); nakama at {NAKAMA_HOST}:7350")
            results += scenario_boot(guest, args.dll, out, args, login=True)
    except (EnvError, RuntimeError) as e:
        # RuntimeError is a failed guest/SMB command: the rig broke, not the runtime.
        print(f"ENV: {e}", file=sys.stderr)
        return 2
    finally:
        if guest is not None:
            try:
                kill_game(guest)
                guest.ps("Unregister-ScheduledTask -TaskName nevrsystest,nevrwin -Confirm:$false -ErrorAction SilentlyContinue; exit 0")
            except RuntimeError as e:
                print(f"cleanup failed: {e}", file=sys.stderr)

    width = max(len(r.name) for r in results) if results else 0
    for r in results:
        print(f"{r.status:5s} {r.name:<{width}s}  {r.detail}")
    (out / "results.txt").write_text("\n".join(f"{r.status} {r.name} {r.detail}" for r in results) + "\n")
    ok = checks.overall(results)
    print("RESULT:", "PASS" if ok else "FAIL")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
