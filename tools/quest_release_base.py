#!/usr/bin/env python3
"""The base a release Quest APK is repacked from, and the check that the release APK kept every entry of it (#1994).

WHY. The release Quest APK carries the reference build's combat content: about a hundred script libraries
(`lib/arm64-v8a/lib<16 hex>.so`), `lib/arm64-v8a/libassetpatch.so`, the reference runtime's `libvrapi.so` and
`assets/afk_default_on.txt`. The store APK has none of them. v6.12.0 was repacked from the reference build and has combat;
v6.16.0 to v6.16.3 were repacked from the store APK and have none, and nothing in the release path noticed. This tool
makes the base a fixed input and checks the result.

WHAT.

    quest_release_base.py prepare --apk REFERENCE.apk --out BASE.apk
        Refuses unless REFERENCE.apk is the pinned reference APK (REFERENCE_APK_SHA256). Writes BASE.apk: the reference
        APK with its package renamed to com.readyatdawn.r15 and the launcher label "Echo VR" (quest_apk_rename_package.py,
        the `just quest-reference-base` step), and refuses unless BASE.apk holds every REFERENCE_ENTRIES name.

    quest_release_base.py check-arena --apk ARENA.apk
        Refuses unless ARENA.apk (the store APK repacked with the release's loader, #2298) holds none of REFERENCE_ENTRIES and no
        plugin manifest, and its variant stamp is exactly `arena`.

    quest_release_base.py check-release --apk RELEASE.apk --base BASE.apk
        Refuses unless BASE.apk holds every REFERENCE_ENTRIES name and RELEASE.apk holds every entry name of BASE.apk
        outside META-INF/ (the repack replaces and adds entries; it never drops one). A refusal names every missing entry.

One JSON line on standard output per run (tag NEVR.QUEST.BASE: the event, the paths, the sha256 and the counts); a refusal
is one JSON line on standard error and exit status 1. Nothing here signs, uploads or prints a credential.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import sys
import zipfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import quest_apk_rename_package as qr  # noqa: E402

TAG = "NEVR.QUEST.BASE"

# The reference combat build, as shipped (package com.readyatdawn.afk, versionCode 4987566). v6.12.0 and v6.16.4 were
# repacked from it.
REFERENCE_APK_SHA256 = "d0b51a014647643a30469ed318968b17374f39286b7d9ded5723f39299eedb48"

# Entries the reference build carries and the store APK lacks: a base without them is not the reference build.
REFERENCE_ENTRIES = ("assets/afk_default_on.txt", "lib/arm64-v8a/libassetpatch.so")

LABEL = "Echo VR"

# What an arena APK must not carry besides REFERENCE_ENTRIES (#2298): the manifest that lists packaged plugins. Every release recipe that
# packaged a plugin library wrote this manifest with it, so an APK that carries a plugin library carries the manifest too.
ARENA_FORBIDDEN_ENTRIES = ("lib/arm64-v8a/libnevr_plugin_manifest.so",)
ARENA_STAMP_ENTRY = "lib/arm64-v8a/libnevr_build_variant.so"


class Refused(Exception):
    def __init__(self, event: str, **fields):
        super().__init__(event)
        self.event = event
        self.fields = fields


def _sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            digest.update(chunk)
    return digest.hexdigest()


def _names(path: Path) -> set:
    try:
        with zipfile.ZipFile(path) as z:
            return {n for n in z.namelist() if not n.endswith("/")}
    except (OSError, zipfile.BadZipFile) as error:
        raise Refused("unreadable_apk", apk=str(path), error=type(error).__name__) from error


def _outside_meta_inf(names: set) -> set:
    return {n for n in names if not n.startswith("META-INF/")}


def _require_reference_entries(base: Path, names: set) -> None:
    missing = sorted(n for n in REFERENCE_ENTRIES if n not in names)
    if missing:
        raise Refused("base_not_reference", base=str(base), missing=missing)


def prepare(apk: Path, out: Path, expected_sha256: str = REFERENCE_APK_SHA256, rename=qr.rename_apk) -> dict:
    if not apk.is_file():
        raise Refused("reference_apk_missing", apk=str(apk))
    sha = _sha256(apk)
    if sha != expected_sha256:
        raise Refused("not_the_reference_apk", apk=str(apk), sha256=sha, expected_sha256=expected_sha256)
    out.parent.mkdir(parents=True, exist_ok=True)
    try:
        rename(apk, out, label=LABEL)
    except (OSError, zipfile.BadZipFile, qr.RenameError) as error:
        raise Refused("rename_refused", apk=str(apk), error=str(error)) from error
    names = _names(out)
    try:
        _require_reference_entries(out, names)
    except Refused:
        out.unlink(missing_ok=True)  # a refused base is not left where the repack would take it
        raise
    return {"event": "base_prepared", "apk": str(apk), "sha256": sha, "base": str(out), "base_sha256": _sha256(out),
            "entries": len(names)}


def check_release(apk: Path, base: Path) -> dict:
    base_names = _names(base)
    _require_reference_entries(base, base_names)
    if _sha256(apk) == _sha256(base):
        raise Refused("release_is_the_base", apk=str(apk), base=str(base))
    release_names = _names(apk)
    missing = sorted(_outside_meta_inf(base_names) - release_names)
    if missing:
        raise Refused("release_lacks_base_entries", apk=str(apk), base=str(base), missing_count=len(missing),
                      missing=missing)
    return {"event": "release_keeps_base", "apk": str(apk), "base": str(base),
            "base_entries": len(_outside_meta_inf(base_names)), "release_entries": len(release_names)}


def check_arena(apk: Path) -> dict:
    """The arena APK carries no combat (#2298): none of the reference build's entries, no plugin manifest, and the
    variant stamp `arena` (the exact bytes). A refusal names every offending entry."""
    try:
        with zipfile.ZipFile(apk) as z:
            names = {n for n in z.namelist() if not n.endswith("/")}
            stamp = z.read(ARENA_STAMP_ENTRY) if ARENA_STAMP_ENTRY in names else None
    except (OSError, zipfile.BadZipFile) as error:
        raise Refused("unreadable_apk", apk=str(apk), error=type(error).__name__) from error
    combat = sorted(n for n in names if n in REFERENCE_ENTRIES or n in ARENA_FORBIDDEN_ENTRIES)
    if combat:
        raise Refused("arena_has_combat_entries", apk=str(apk), entries=combat)
    if stamp != b"arena":
        raise Refused("arena_stamp_wrong", apk=str(apk), stamp="absent" if stamp is None else stamp.decode("ascii", "replace")[:32])
    return {"event": "arena_has_no_combat", "apk": str(apk), "entries": len(names)}


def _emit(stream, record: dict) -> None:
    stream.write(json.dumps({"tag": TAG, **record}, sort_keys=True) + "\n")


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    sub = parser.add_subparsers(dest="command", required=True)
    p = sub.add_parser("prepare")
    p.add_argument("--apk", type=Path, required=True)
    p.add_argument("--out", type=Path, required=True)
    a = sub.add_parser("check-arena")
    a.add_argument("--apk", type=Path, required=True)
    c = sub.add_parser("check-release")
    c.add_argument("--apk", type=Path, required=True)
    c.add_argument("--base", type=Path, required=True)
    args = parser.parse_args(argv)
    try:
        if args.command == "prepare":
            record = prepare(args.apk, args.out)
        elif args.command == "check-arena":
            record = check_arena(args.apk)
        else:
            record = check_release(args.apk, args.base)
    except Refused as refusal:
        _emit(sys.stderr, {"event": refusal.event, **refusal.fields})
        return 1
    _emit(sys.stdout, record)
    return 0


if __name__ == "__main__":
    sys.exit(main())
