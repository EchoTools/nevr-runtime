"""Shared by the installer tests: build the core, and a throwaway signed world for it to install from.

A World is a minisign key (pinned in its own pins.txt), a manifest signed with it, the Windows package that manifest
names, and a local HTTP server on 127.0.0.1 that serves both. Nothing here touches a real game install or the network.
"""
from __future__ import annotations

import hashlib
import http.server
import io
import json
import os
import pathlib
import subprocess
import threading
import zipfile

REPO = pathlib.Path(__file__).resolve().parents[2]
HOST = b"new host NEVR-BUILD 9.9.9"
RUNTIME = b"new runtime NEVR-BUILD 9.9.9"
GAME = b"the game's own BugSplat64.dll"
_CORE: list = []


def sha(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def core_binary() -> pathlib.Path:
    """Build the core once per run (locked) and return the path of the binary."""
    if _CORE:
        return _CORE[0]
    target = pathlib.Path(os.environ.get("CARGO_TARGET_DIR") or REPO / "installer/target")
    built = subprocess.run(["cargo", "build", "--locked", "--manifest-path", str(REPO / "installer/Cargo.toml"),
                            "--bin", "install-nevr-runtime"], capture_output=True, text=True, timeout=1500)
    if built.returncode != 0:
        raise RuntimeError("cargo build of installer/ failed:\n" + built.stderr[-2000:])
    _CORE.append(target / "debug/install-nevr-runtime")
    return _CORE[0]


class Server:
    """Serves a directory over HTTP on 127.0.0.1 on a free port."""

    def __init__(self, root: pathlib.Path):
        handler = type("H", (http.server.SimpleHTTPRequestHandler,),
                       {"__init__": lambda s, *a, **k: http.server.SimpleHTTPRequestHandler.__init__(s, *a, directory=str(root), **k),
                        "log_message": lambda *a, **k: None})
        self.httpd = http.server.ThreadingHTTPServer(("127.0.0.1", 0), handler)
        self.base = f"http://127.0.0.1:{self.httpd.server_address[1]}"
        threading.Thread(target=self.httpd.serve_forever, daemon=True).start()

    def close(self):
        self.httpd.shutdown()
        self.httpd.server_close()


class World:
    """A signed manifest (key pinned in pins.txt), the Windows package it names, and the server that serves both."""

    def __init__(self, tmp: pathlib.Path, tamper: bool = False, host: bytes = HOST, runtime: bytes = RUNTIME,
                 manifest_version: int = 5, name: str = "w", serve_zip: bool = True):
        """`name` separates several worlds in one `tmp`; a later world needs a higher `manifest_version`, because the
        core refuses a manifest with the same version and other bytes. `serve_zip` False leaves the package off the
        server, so a run that downloads it fails."""
        self.tmp = tmp = tmp / name
        tmp.mkdir()
        site = tmp / "site"
        (site / "v1.0.0").mkdir(parents=True)
        keys = tmp / "keys"
        keys.mkdir()
        subprocess.run(["minisign", "-G", "-W", "-f", "-p", str(keys / "k.pub"), "-s", str(keys / "k.key")],
                       check=True, capture_output=True)
        pub = (keys / "k.pub").read_text().splitlines()[1]
        self.pins = tmp / "pins.txt"
        self.pins.write_text(f"T {pub}\n")
        buf = io.BytesIO()
        with zipfile.ZipFile(buf, "w") as z:
            z.writestr("BugSplat64.dll", host)
            z.writestr("nevr-runtime.dll", runtime)
        zip_bytes = buf.getvalue()
        if serve_zip:
            (site / "v1.0.0/nevr-runtime-v1.0.0-windows.zip").write_bytes(zip_bytes)
        manifest = {
            "schema_version": 1, "product": "nevr-runtime", "manifest_version": manifest_version, "expires": "2099-01-01T00:00:00Z",
            "channels": {"beta": "1.0.0"},
            "releases": {"1.0.0": {"artifacts": [{
                "kind": "windows-zip", "platform": "windows-x86_64", "path": "v1.0.0/nevr-runtime-v1.0.0-windows.zip",
                "size": len(zip_bytes), "sha256": sha(zip_bytes),
                "contents": [{"path": "BugSplat64.dll", "sha256": sha(host), "size": len(host)},
                             {"path": "nevr-runtime.dll", "sha256": sha(runtime), "size": len(runtime)}]}]}},
        }
        body = json.dumps(manifest).encode()
        (site / "manifest.json").write_bytes(body)
        subprocess.run(["minisign", "-S", "-s", str(keys / "k.key"), "-x", str(site / "manifest.json.sig"),
                        "-m", str(site / "manifest.json")], check=True, capture_output=True)
        if tamper:  # serve different bytes than were signed
            (site / "manifest.json").write_bytes(body.replace(b"beta", b"bata"))
        self.server = Server(site)
        self.root = tmp / "core-root"
        self.nevr = tmp / "nevr-root"  # the player's nevr folder: runtime, install record and set-aside files (ADR 0012)
        self.game = tmp / "game"
        self.game.mkdir()
        (self.game / "echovr.exe").write_bytes(b"exe")
        (self.game / "BugSplat64.dll").write_bytes(GAME)

    def core_args(self) -> list[str]:
        # The nevr folder is a temporary one, not %LOCALAPPDATA%\nevr\echovr: a dev run says so (#1353).
        return ["--root", str(self.root), "--base", self.server.base, "--pins", str(self.pins), "--dev-nevr-root"]

    def game_files(self) -> list[str]:
        """What the game folder holds, by name: the standing rule is that only BugSplat64.dll of ours is ever in it."""
        return sorted(p.name for p in self.game.iterdir())

    def records(self) -> list[dict]:
        log = self.root / "logs/nevr-install.jsonl"
        return [json.loads(line) for line in log.read_text().splitlines()] if log.exists() else []

    def events(self) -> list[str]:
        return [r["event"] for r in self.records()]
