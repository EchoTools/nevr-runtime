#!/usr/bin/env python3
"""The headset half of the store build (`just quest-store-install`, docs/adr/0042-quest-store-variant-built-from-store-apk-and-obb.md).

Reads build/quest-store/store-build.json (written by `just quest-store-build`), checks the APK and the `_data` tree it names
against their digests, and then, on exactly one attached headset:

  1. `adb install -r` of the signed APK;
  2. pushes `_data` to a staging folder beside the game's data folder, hashes every pushed file on the device, and only then
     replaces `/sdcard/Android/media/com.readyatdawn.r15/files/_data` with it.

Every adb call is one JSON line (tag NEVR.QUESTBUILD) with its argument list, exit code and time; a refusal is a line with the event
`refused`, a `code` and a `detail`, exit 1. Nothing is uninstalled: an install the headset refuses (a different signer, an older
version code) is reported with adb's own words, and the data is left as it was.

  quest_store_install.py [--serial SERIAL] [--root REPOSITORY_ROOT]
"""
from __future__ import annotations

import argparse
import datetime
import json
import pathlib
import shutil
import subprocess
import sys
import time
from typing import Dict, List, Optional

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))  # -I leaves the script's folder off the path
import quest_store_build as build  # noqa: E402

TAG = build.TAG
PACKAGE = "com.readyatdawn.r15"
DEVICE_FILES = f"/sdcard/Android/media/{PACKAGE}/files"
DEVICE_STAGING = f"{DEVICE_FILES}/.nevr-stock-staging"
INSTALL_TIMEOUT_S = 600
PUSH_TIMEOUT_S = 1800
SHELL_TIMEOUT_S = 120
HASH_TIMEOUT_S = 600
EXIT_REFUSED = 1


class Adb:
    """adb, one call at a time, every call logged. `serial` is passed as -s when given."""

    def __init__(self, program: str, serial: Optional[str]):
        self.program = program
        self.serial = serial

    def run(self, event: str, args: List[str], timeout: int) -> subprocess.CompletedProcess:
        argv = [self.program] + (["-s", self.serial] if self.serial else []) + args
        started = time.monotonic()
        try:
            done = subprocess.run(argv, capture_output=True, text=True, timeout=timeout)
        except subprocess.TimeoutExpired:
            build.log("error", event, argv=["adb"] + args, timeout_s=timeout, outcome="timeout")
            raise build.Refused("adb_timeout", f"adb {' '.join(args[:2])} did not finish in {timeout} s; the headset step stopped there")
        tail = (done.stdout.strip() + " " + done.stderr.strip()).strip()[-300:]
        build.log("info" if done.returncode == 0 else "warning", event, argv=["adb"] + args, exit_code=done.returncode,
                  ms=round((time.monotonic() - started) * 1000, 1), output_tail=tail)
        return done

    def shell(self, event: str, command: List[str], timeout: int = SHELL_TIMEOUT_S) -> subprocess.CompletedProcess:
        return self.run(event, ["shell"] + command, timeout)


def read_manifest(out: pathlib.Path) -> Dict[str, object]:
    path = out / build.MANIFEST_NAME
    if not path.is_file():
        raise build.Refused("store_build_missing", f"{path} is not there: run `just quest-store-build` first")
    try:
        manifest = json.loads(path.read_text())
        manifest["outputs"]["apk"]["path"], manifest["outputs"]["data"]
    except (ValueError, KeyError, TypeError) as error:
        raise build.Refused("store_build_unreadable", f"{path} does not parse as a store build manifest: {error}")
    return manifest


def check_outputs(layout: build.Layout, manifest: Dict[str, object]) -> None:
    fault = build.verify_outputs(layout, manifest)
    if fault is not None:
        raise build.Refused("store_build_stale", f"build/quest-store does not match its manifest ({fault}): run `just quest-store-build` again")


def choose_device(adb: Adb) -> str:
    done = adb.run("adb_devices", ["devices"], SHELL_TIMEOUT_S)
    if done.returncode != 0:
        raise build.Refused("adb_failed", f"adb devices exited {done.returncode}: {done.stderr.strip()[-200:]}")
    devices = []
    for line in done.stdout.splitlines()[1:]:
        parts = line.split()
        if len(parts) >= 2:
            devices.append((parts[0], parts[1]))
    if adb.serial:
        devices = [d for d in devices if d[0] == adb.serial]
    if not devices:
        raise build.Refused("no_device", "no headset is attached" + (f" with serial {adb.serial}" if adb.serial else "") + ": connect one, with USB debugging allowed")
    if len(devices) > 1:
        raise build.Refused("multiple_devices", f"{len(devices)} devices are attached ({', '.join(d[0] for d in devices)}): pass --serial")
    serial, state = devices[0]
    if state != "device":
        raise build.Refused("device_not_ready", f"{serial} is '{state}', not 'device': allow USB debugging on the headset")
    return serial


def device_sha256(adb: Adb, path: str) -> str:
    done = adb.shell("device_hash", ["sha256sum", path], HASH_TIMEOUT_S)
    words = done.stdout.split()
    if done.returncode != 0 or not words or not build.HEX64.match(words[0]):
        raise build.Refused("device_hash_failed", f"sha256sum {path} on the headset exited {done.returncode}: {done.stderr.strip()[-200:]}")
    return words[0]


def install(out: pathlib.Path, adb: Adb, manifest: Dict[str, object]) -> None:
    apk = out / manifest["outputs"]["apk"]["path"]
    done = adb.run("apk_install", ["install", "-r", str(apk)], INSTALL_TIMEOUT_S)
    if done.returncode != 0 or "Success" not in done.stdout:
        raise build.Refused("install_failed", f"adb install -r exited {done.returncode}: {(done.stdout + done.stderr).strip()[-300:]} (nothing was uninstalled and the data is untouched)")
    build.log("info", "apk_installed", apk=apk.name, sha256=manifest["outputs"]["apk"]["sha256"])


def replace_data(out: pathlib.Path, adb: Adb, manifest: Dict[str, object]) -> None:
    files = manifest["outputs"]["data"]
    for step, command in (("staging_cleared", ["rm", "-rf", DEVICE_STAGING]), ("staging_made", ["mkdir", "-p", DEVICE_STAGING])):
        done = adb.shell(step, command)
        if done.returncode != 0:
            raise build.Refused("adb_failed", f"{' '.join(command)} exited {done.returncode}: {done.stderr.strip()[-200:]}")
    done = adb.run("data_push", ["push", str(out / "_data"), DEVICE_STAGING + "/"], PUSH_TIMEOUT_S)
    if done.returncode != 0:
        raise build.Refused("push_failed", f"adb push exited {done.returncode}: {(done.stdout + done.stderr).strip()[-300:]}")
    try:
        for entry in files:
            measured = device_sha256(adb, f"{DEVICE_STAGING}/{entry['path']}")
            if measured != entry["sha256"]:
                raise build.Refused("device_digest_mismatch", f"{entry['path']} is {measured} on the headset; the build made {entry['sha256']}")
        build.log("info", "device_data_verified", files=len(files), where="staging")
    except build.Refused:
        adb.shell("staging_removed", ["rm", "-rf", DEVICE_STAGING])
        raise
    for step, command in (("data_removed", ["rm", "-rf", f"{DEVICE_FILES}/_data"]),
                          ("data_moved", ["mv", f"{DEVICE_STAGING}/_data", f"{DEVICE_FILES}/_data"]),
                          ("staging_removed", ["rm", "-rf", DEVICE_STAGING])):
        done = adb.shell(step, command)
        if done.returncode != 0:
            raise build.Refused("adb_failed", f"{' '.join(command)} exited {done.returncode}: {done.stderr.strip()[-200:]}")
    for entry in files:
        done = adb.shell("device_size", ["stat", "-c", "%s", f"{DEVICE_FILES}/{entry['path']}"])
        if done.returncode != 0 or done.stdout.strip() != str(entry["bytes"]):
            raise build.Refused("device_data_missing", f"{DEVICE_FILES}/{entry['path']} is not {entry['bytes']} bytes on the headset after the move")
    build.log("info", "data_replaced", directory=f"{DEVICE_FILES}/_data", files=len(files), bytes=sum(e["bytes"] for e in files))


def run(root: pathlib.Path, serial: Optional[str]) -> int:
    started = time.monotonic()
    layout = build.Layout(root.resolve())
    out = layout.out
    program = shutil.which("adb")
    if program is None:
        raise build.Refused("adb_missing", "adb is not on PATH: install Android platform-tools")
    manifest = read_manifest(out)
    check_outputs(layout, manifest)
    build.log("info", "store_build_read", key=manifest["key"], apk_sha256=manifest["outputs"]["apk"]["sha256"], data_files=len(manifest["outputs"]["data"]))
    adb = Adb(program, serial)
    device = choose_device(adb)
    adb.serial = device
    build.log("info", "device_chosen", serial=device)
    install(out, adb, manifest)
    replace_data(out, adb, manifest)
    build.log("info", "store_install_done", serial=device, ms=round((time.monotonic() - started) * 1000, 1))
    return 0


def main(argv: List[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--serial", default=None, help="the headset's adb serial; required when more than one device is attached")
    parser.add_argument("--root", type=pathlib.Path, default=build.REPO, help="repository root (tests pass a scratch tree)")
    try:
        args = parser.parse_args(argv)
    except SystemExit as exit_:
        return 2 if exit_.code else 0
    try:
        return run(args.root, args.serial)
    except build.Refused as refusal:
        build.log("error", "refused", code=refusal.code, detail=refusal.detail)
        return EXIT_REFUSED
    except (OSError, subprocess.SubprocessError) as error:
        build.log("error", "refused", code="failed", detail=f"{type(error).__name__}: {error}")
        return EXIT_REFUSED


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
