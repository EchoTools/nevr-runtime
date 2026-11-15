#!/usr/bin/env python3
"""The headset half of the store build (`just quest-store-install`, docs/adr/0042-quest-store-variant-built-from-store-apk-and-obb.md).

Reads build/quest-store/store-build.json (written by `just quest-store-build`), checks the APK and the `_data` tree it names
against their digests, and then, on exactly one attached headset:

  1. `adb install -r` of the signed APK;
  2. pushes `_data` to a staging folder beside the game's data folder, hashes every pushed file on the device, and only then
     replaces `/sdcard/Android/media/com.readyatdawn.r15/files/_data` with it.

Removal on the headset follows ORDER.md "The manifest rule": only the paths the build manifest lists, or the record the previous
install left (`files/_data.files`, one path per line, written beside `_data`), are removed, each by its literal path with
`rm -f --`; a folder goes only with `rmdir`, so only when it is empty. A folder that still holds something unlisted is moved aside to
`_data.unrecorded/<UTC stamp>/` (one unique folder per set-aside, so it never collides with an earlier one). Nothing is refused for
the number of set-asides: the install logs a warning with the count (old_copies_kept), so the player can remove them. A record line that is now a folder is skipped with a
logged warning (the rmdir pass handles folders). Nothing is removed with `rm -rf`.

Every adb call is one JSON line (tag NEVR.QUESTBUILD) with its argument list, exit code and time; a refusal is a line with the event
`refused`, a `code` and a `detail`, exit 1. Nothing is uninstalled: an install the headset refuses (a different signer, an older
version code) is reported with adb's own words, and the data is left as it was.

  quest_store_install.py [--serial SERIAL] [--root REPOSITORY_ROOT]
"""
from __future__ import annotations

import argparse
import datetime
import json
import os
import pathlib
import secrets
import shlex
import shutil
import subprocess
import sys
import tempfile
import time
from typing import Dict, List, Optional

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))  # -I leaves the script's folder off the path
import quest_store_build as build  # noqa: E402

TAG = build.TAG
PACKAGE = "com.readyatdawn.r15"
DEVICE_FILES = f"/sdcard/Android/media/{PACKAGE}/files"
DEVICE_STAGING = f"{DEVICE_FILES}/.nevr-stock-staging"
DEVICE_DATA = f"{DEVICE_FILES}/_data"
DEVICE_RECORD = f"{DEVICE_FILES}/_data.files"
RECORD_NAME = "_data.files"
DATA_PREFIX = "_data/"
DATA_ASIDE_ROOT = f"{DEVICE_FILES}/_data.unrecorded"
STAGING_ASIDE_ROOT = f"{DEVICE_FILES}/.nevr-stock-staging.unrecorded"
RECORD_MAX_BYTES = 1024 * 1024
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


def valid_data_entry(entry: object) -> bool:
    """A path the install may remove on the headset. The first half is ValidEntry (src/core/owned_remove_rules.h): a non-empty
    relative '/'-separated name, no control byte, no '\\' and no ':', no empty, '.' or '..' component. The second half is the
    install's own rule: only paths under _data/ are the install's to remove."""
    if not isinstance(entry, str) or not entry.startswith(DATA_PREFIX):
        return False
    if any(ord(c) < 0x20 or ord(c) == 0x7F for c in entry) or "\\" in entry or ":" in entry:
        return False
    return all(part not in ("", ".", "..") for part in entry.split("/"))


def check_entries(manifest: Dict[str, object]) -> None:
    for entry in manifest["outputs"]["data"]:
        path = entry.get("path") if isinstance(entry, dict) else None
        if not valid_data_entry(path):
            raise build.Refused("bad_manifest_path", f"{path!r} is not a path under {DATA_PREFIX}: the install removes only listed paths under {DEVICE_FILES}, and this manifest is refused")


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
    done = adb.shell("device_hash", ["sha256sum", shlex.quote(path)], HASH_TIMEOUT_S)
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


def device_exists(adb: Adb, path: str) -> bool:
    return adb.shell("device_exists", ["test", "-e", shlex.quote(path)]).returncode == 0


def device_is_folder(adb: Adb, path: str) -> bool:
    return adb.shell("device_is_folder", ["test", "-d", shlex.quote(path)]).returncode == 0


def device_names(adb: Adb, folder: str) -> List[str]:
    done = adb.shell("device_list", ["ls", "-1", shlex.quote(folder)])
    return [] if done.returncode != 0 else [line for line in done.stdout.splitlines() if line]


def old_copies(adb: Adb) -> int:
    """How many set-aside folders the earlier installs left, read only: one per entry under each aside root."""
    return sum(len(device_names(adb, root)) for root in (DATA_ASIDE_ROOT, STAGING_ASIDE_ROOT))


def new_aside(root: str) -> str:
    """A new, unique folder under `root` for one set-aside: <root>/<UTC stamp>-<token>. The folder is made first, so the move has a parent."""
    stamp = datetime.datetime.now(datetime.timezone.utc).strftime("%Y%m%dT%H%M%S%fZ")
    return f"{root}/{stamp}-{secrets.token_hex(2)}"


def read_record(adb: Adb) -> Optional[List[str]]:
    """The paths the previous install left in its record; None when there is no record (or it cannot be read: the manifest's list is
    then the only one used). A record over RECORD_MAX_BYTES, or a line that is not a valid data path, refuses the install before
    anything is touched."""
    size = adb.shell("record_size", ["stat", "-c", "%s", shlex.quote(DEVICE_RECORD)])
    if size.returncode != 0:
        return None
    if not size.stdout.strip().isdigit() or int(size.stdout.strip()) > RECORD_MAX_BYTES:
        raise build.Refused("bad_record", f"{DEVICE_RECORD} is larger than {RECORD_MAX_BYTES} bytes or unreadable; nothing on the headset was changed")
    done = adb.shell("record_read", ["cat", shlex.quote(DEVICE_RECORD)])
    if done.returncode != 0:
        return None
    names = [line for line in done.stdout.splitlines() if line != ""]
    for name in names:
        if not valid_data_entry(name):
            raise build.Refused("bad_record", f"{DEVICE_RECORD} lists {name!r}, which is not a path under {DATA_PREFIX}; nothing on the headset was changed")
    return names


def parents_deepest_first(names: List[str]) -> List[str]:
    found = set()
    for name in names:
        parts = name.split("/")
        for i in range(1, len(parts)):
            found.add("/".join(parts[:i]))
    return sorted(found, key=lambda d: (-d.count("/"), d))


def clear_folder(adb: Adb, folder: str, names: List[str], aside_root: str, label: str) -> None:
    """Removes, on the headset, only `names` (paths relative to `folder`, each one listed by the manifest or a record): each file by its
    literal path with `rm -f --`, then the folders those files were in, deepest first, each with `rmdir` (a folder that is not empty
    stays), then `folder` itself with `rmdir`. A name that is now a folder is skipped with a logged warning: the rmdir pass handles
    folders. When `folder` still exists it holds something unlisted: it is moved to a new unique folder under `aside_root`."""
    for name in names:
        path = f"{folder}/{name}"
        done = adb.shell(label + "_file_removed", ["rm", "-f", "--", shlex.quote(path)])
        if done.returncode != 0:
            if device_is_folder(adb, path):
                build.log("warning", label + "_folder_skipped", path=path, reason="a folder: the rmdir pass handles folders")
                continue
            raise build.Refused("adb_failed", f"rm -f {path} exited {done.returncode}: {done.stderr.strip()[-200:]}")
    for name in parents_deepest_first(names):
        adb.shell(label + "_folder_removed", ["rmdir", "--", shlex.quote(f"{folder}/{name}")])  # not empty: what is in it stays
    adb.shell(label + "_folder_removed", ["rmdir", "--", shlex.quote(folder)])
    if not device_exists(adb, folder):
        return
    aside = new_aside(aside_root)
    made = adb.shell(label + "_aside_root", ["mkdir", "-p", shlex.quote(aside_root)])
    if made.returncode != 0:
        raise build.Refused("adb_failed", f"mkdir -p {aside_root} exited {made.returncode}: {made.stderr.strip()[-200:]}")
    done = adb.shell(label + "_moved_aside", ["mv", shlex.quote(folder), shlex.quote(aside)])
    if done.returncode != 0:
        raise build.Refused("adb_failed", f"mv {folder} {aside} exited {done.returncode}: {done.stderr.strip()[-200:]}")
    build.log("warning", label + "_moved_aside", folder=folder, aside=aside, reason="holds files no record or manifest lists")


def write_local_record(text: str) -> str:
    fd, path = tempfile.mkstemp(prefix="nevr-record-", suffix=".txt")
    with os.fdopen(fd, "w") as handle:
        handle.write(text)
    return path


def replace_data(out: pathlib.Path, adb: Adb, manifest: Dict[str, object]) -> None:
    files = manifest["outputs"]["data"]
    entries = [entry["path"] for entry in files]
    recorded = read_record(adb)  # refuses before anything is touched
    old_names = recorded if recorded is not None else entries
    staging_names = entries + [RECORD_NAME]
    clear_folder(adb, DEVICE_STAGING, staging_names, STAGING_ASIDE_ROOT, "staging")
    done = adb.shell("staging_made", ["mkdir", "-p", shlex.quote(DEVICE_STAGING)])
    if done.returncode != 0:
        raise build.Refused("adb_failed", f"mkdir -p {DEVICE_STAGING} exited {done.returncode}: {done.stderr.strip()[-200:]}")

    def abandon(code: str, detail: str) -> None:
        clear_folder(adb, DEVICE_STAGING, staging_names, STAGING_ASIDE_ROOT, "staging")  # only what this install pushed
        raise build.Refused(code, detail)

    done = adb.run("data_push", ["push", str(out / "_data"), DEVICE_STAGING + "/"], PUSH_TIMEOUT_S)
    if done.returncode != 0:
        abandon("push_failed", f"adb push exited {done.returncode}: {(done.stdout + done.stderr).strip()[-300:]} (the data in use is untouched)")
    local_record = write_local_record("".join(name + "\n" for name in sorted(entries)))
    try:
        done = adb.run("record_push", ["push", local_record, f"{DEVICE_STAGING}/{RECORD_NAME}"], PUSH_TIMEOUT_S)
    finally:
        os.unlink(local_record)
    if done.returncode != 0:
        abandon("push_failed", f"adb push of the record exited {done.returncode}: {(done.stdout + done.stderr).strip()[-300:]} (the data in use is untouched)")
    try:
        for entry in files:
            measured = device_sha256(adb, f"{DEVICE_STAGING}/{entry['path']}")
            if measured != entry["sha256"]:
                raise build.Refused("device_digest_mismatch", f"{entry['path']} is {measured} on the headset; the build made {entry['sha256']}")
        build.log("info", "device_data_verified", files=len(files), where="staging")
    except build.Refused as refusal:
        abandon(refusal.code, refusal.detail)
    clear_folder(adb, DEVICE_DATA, [name[len(DATA_PREFIX):] for name in old_names], DATA_ASIDE_ROOT, "data")
    for step, source, target in (("data_moved", f"{DEVICE_STAGING}/_data", DEVICE_DATA),
                                 ("record_moved", f"{DEVICE_STAGING}/{RECORD_NAME}", DEVICE_RECORD)):
        done = adb.shell(step, ["mv", shlex.quote(source), shlex.quote(target)])
        if done.returncode != 0:
            raise build.Refused("adb_failed", f"mv {source} {target} exited {done.returncode}: {done.stderr.strip()[-200:]}")
    clear_folder(adb, DEVICE_STAGING, [], STAGING_ASIDE_ROOT, "staging")  # the emptied folder; anything left in it is moved aside
    for entry in files:
        done = adb.shell("device_size", ["stat", "-c", "%s", shlex.quote(f"{DEVICE_FILES}/{entry['path']}")])
        if done.returncode != 0 or done.stdout.strip() != str(entry["bytes"]):
            raise build.Refused("device_data_missing", f"{DEVICE_FILES}/{entry['path']} is not {entry['bytes']} bytes on the headset after the move")
    copies = old_copies(adb)
    build.log("info", "data_replaced", directory=DEVICE_DATA, files=len(files), bytes=sum(e["bytes"] for e in files),
              previous_record=recorded is not None, previous_names=len(old_names))
    if copies:
        build.log("warning", "old_copies_kept", count=copies, roots=[DATA_ASIDE_ROOT, STAGING_ASIDE_ROOT],
                  detail=f"{copies} old copies kept under {DATA_ASIDE_ROOT}; remove them to free space")


def run(root: pathlib.Path, serial: Optional[str]) -> int:
    started = time.monotonic()
    layout = build.Layout(root.resolve())
    out = layout.out
    program = shutil.which("adb")
    if program is None:
        raise build.Refused("adb_missing", "adb is not on PATH: install Android platform-tools")
    manifest = read_manifest(out)
    check_entries(manifest)
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
