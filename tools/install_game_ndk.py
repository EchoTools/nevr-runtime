#!/usr/bin/env python3
"""Install the game's NDK (r21b), pinned by tools/game_ndk.pin, outside the checkout (`just toolchain-r21b`).

  python3 -I tools/install_game_ndk.py [--pin FILE] [--root DIR] [--print-dir]

Steps: read the pin -> download the archive once (kept in <root>/downloads/) -> verify its sha256 against the pin ->
extract into a temporary directory inside <root> -> gate on the first line of `clang --version` equalling the pin's
CLANG_VERSION exactly -> rename into place as <root>/ndk-<release>. A run that finds the toolchain in place re-verifies
it (the cached archive's sha256, when the archive is still there, and the clang line) and changes nothing.

<root> is --root, else $XDG_CACHE_HOME/nevr-runtime/toolchain, else ~/.cache/nevr-runtime/toolchain: a directory the repo's
tools own and every worktree and checkout of one user shares.

Every step is one JSON line, appended to <root>/ndk-install.log and echoed to stdout (stderr for ERROR). Any failure exits
1 with the cause in the last line; nothing is left half-installed. --print-dir prints <root>/ndk-<release> and exits 0
when that directory exists, and exits 1 (nothing on stdout) when it does not.
"""

import argparse
import datetime
import hashlib
import json
import os
import pathlib
import shutil
import subprocess
import sys
import tempfile
import urllib.error
import urllib.request

HERE = pathlib.Path(__file__).resolve().parent
KEYS = ("NDK_RELEASE", "NDK_BUILD", "NDK_URL", "NDK_SHA256", "CLANG_VERSION")
CLANG_REL = pathlib.Path("toolchains/llvm/prebuilt/linux-x86_64/bin/clang")
DOWNLOAD_ATTEMPTS = 3
SOCKET_TIMEOUT_S = 60  # per read; a stalled transfer fails the attempt, and the attempt is retried
UNZIP_TIMEOUT_S = 900
CLANG_TIMEOUT_S = 60


class Failed(Exception):
    """A step failed; the ERROR record is already written."""


class Log:
    def __init__(self, path):
        self.path = path

    def write(self, level, event, **fields):
        record = {"ts": datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"), "level": level,
                  "component": "toolchain-ndk", "event": event}
        record.update({k: str(v) for k, v in fields.items()})
        line = json.dumps(record)
        try:
            self.path.parent.mkdir(parents=True, exist_ok=True)
            with open(self.path, "a", encoding="utf-8") as f:
                f.write(line + "\n")
        except OSError as err:
            print(json.dumps({"level": "ERROR", "component": "toolchain-ndk", "event": "log_write_failed",
                              "path": str(self.path), "error": str(err)}), file=sys.stderr)
            raise Failed() from err
        print(line, file=sys.stderr if level == "ERROR" else sys.stdout, flush=True)

    def info(self, event, **fields):
        self.write("INFO", event, **fields)

    def die(self, event, **fields):
        self.write("ERROR", event, **fields)
        raise Failed()


def default_root():
    cache = os.environ.get("XDG_CACHE_HOME") or ""
    if not cache:
        home = os.environ.get("HOME") or ""
        if not home:
            print("install_game_ndk: neither XDG_CACHE_HOME nor HOME is set; pass --root", file=sys.stderr)
            sys.exit(1)
        cache = os.path.join(home, ".cache")
    return pathlib.Path(cache) / "nevr-runtime" / "toolchain"


def read_pin(log, path):
    if not path.is_file():
        log.die("pin_missing", path=path)
    pin = {}
    for line in path.read_text(encoding="utf-8").splitlines():
        if not line.strip() or line.startswith("#"):
            continue
        key, _, value = line.partition("=")
        if key not in KEYS:
            log.die("pin_unknown_key", key=key, path=path)
        pin[key] = value
    for key in KEYS:
        if not pin.get(key):
            log.die("pin_incomplete", missing=key, path=path)
    return pin


def sha256_of(path):
    digest = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            digest.update(chunk)
    return digest.hexdigest()


def check_archive(log, pin, path):
    actual = sha256_of(path)
    if actual != pin["NDK_SHA256"]:
        log.die("sha256_mismatch", expected_sha256=pin["NDK_SHA256"], actual_sha256=actual, file=path)
    log.info("sha256_ok", sha256=actual, bytes=path.stat().st_size, file=path)


def check_clang(log, pin, clang):
    if not os.access(clang, os.X_OK):
        log.die("clang_missing", path=clang)
    try:
        run = subprocess.run([str(clang), "--version"], capture_output=True, text=True, timeout=CLANG_TIMEOUT_S)
    except (OSError, subprocess.TimeoutExpired) as err:
        log.die("clang_run_failed", path=clang, error=err)
    if run.returncode != 0:
        log.die("clang_run_failed", path=clang, exit=run.returncode)
    first = (run.stdout + run.stderr).split("\n", 1)[0]
    if first != pin["CLANG_VERSION"]:
        log.die("clang_version_mismatch", expected=pin["CLANG_VERSION"], actual=first, path=clang)
    log.info("clang_version_ok", version=first, path=clang)


def download(log, pin, target):
    log.info("download_start", url=pin["NDK_URL"], attempts=DOWNLOAD_ATTEMPTS, timeout_s=SOCKET_TIMEOUT_S)
    last = None
    for attempt in range(1, DOWNLOAD_ATTEMPTS + 1):
        try:
            with urllib.request.urlopen(pin["NDK_URL"], timeout=SOCKET_TIMEOUT_S) as src, open(target, "wb") as dst:
                shutil.copyfileobj(src, dst, 1 << 20)
            return
        except (OSError, urllib.error.URLError, ValueError) as err:
            last = err
            log.write("WARN", "download_attempt_failed", attempt=attempt, error=err)
    log.die("download_failed", url=pin["NDK_URL"], error=last)


def install(log, pin, root):
    release = pin["NDK_RELEASE"]
    dest = root / f"ndk-{release}"
    archive = root / "downloads" / f"android-ndk-{release}-linux-x86_64.zip"
    log.info("run_start", release=release, build=pin["NDK_BUILD"], dest=dest)
    if dest.is_dir():
        log.info("already_present", dest=dest)
        if archive.is_file():
            check_archive(log, pin, archive)
        else:
            log.info("archive_absent", path=archive, note="archive cache removed; toolchain verified by the clang gate only")
        check_clang(log, pin, dest / CLANG_REL)
        log.info("noop_reverified", dest=dest)
        return dest
    root.mkdir(parents=True, exist_ok=True)
    build = pathlib.Path(tempfile.mkdtemp(prefix=".build-", dir=root))
    try:
        (root / "downloads").mkdir(exist_ok=True)
        if archive.is_file():
            log.info("archive_cached", path=archive)
            check_archive(log, pin, archive)
        else:
            fetched = build / "archive.zip"
            download(log, pin, fetched)
            check_archive(log, pin, fetched)
            fetched.replace(archive)
            log.info("download_done", path=archive)
        log.info("extract_start", archive=archive)
        extracted = build / "x"
        extracted.mkdir()
        try:
            unzip = subprocess.run(["unzip", "-q", str(archive), "-d", str(extracted)], capture_output=True, text=True,
                                   timeout=UNZIP_TIMEOUT_S)
        except (OSError, subprocess.TimeoutExpired) as err:
            log.die("extract_failed", archive=archive, error=err)
        if unzip.returncode != 0:
            log.die("extract_failed", archive=archive, exit=unzip.returncode, stderr=unzip.stderr.strip())
        inner = extracted / f"android-ndk-{release}"
        if not inner.is_dir():
            log.die("layout_unexpected", expected_dir=f"android-ndk-{release}")
        check_clang(log, pin, inner / CLANG_REL)
        inner.rename(dest)
        log.info("built", dest=dest)
        return dest
    finally:
        shutil.rmtree(build, ignore_errors=True)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.split("\n", 1)[0])
    parser.add_argument("--pin", type=pathlib.Path, default=HERE / "game_ndk.pin")
    parser.add_argument("--root", type=pathlib.Path, default=None)
    parser.add_argument("--print-dir", action="store_true")
    args = parser.parse_args(argv)
    root = args.root if args.root is not None else default_root()
    log = Log(root / "ndk-install.log")
    try:
        pin = read_pin(log, args.pin)
        if args.print_dir:
            dest = root / f"ndk-{pin['NDK_RELEASE']}"
            if not dest.is_dir():
                log.die("not_installed", dest=dest, note="run `just toolchain-r21b`")
            print(dest)
            return 0
        install(log, pin, root)
    except Failed:
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
