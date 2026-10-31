#!/usr/bin/env python3
"""The package rename of the reference-build APK: `com.readyatdawn.afk` to `com.readyatdawn.r15`, with every count asserted.

WHY. The reference build ships as package `com.readyatdawn.afk`. The sentinel writes its log, its crash directory and
its token store under `com.readyatdawn.r15` (src/quest/sentinel/sentinel_log.cpp, src/quest/sentinel/sentinel.cpp,
src/quest/auth/file_store.h, src/quest/sentinel/data_root.h), and the reference libr15's own data-root strings must
read the same package. An APK that runs as `afk` writes into another app's directory and leaves no log. The patcher's
third accepted input (`kLibR15ReferenceSha256[0]`, src/quest/patch/libr15_patch_table.h) is the reference libr15 with
this rename applied, so the rename is part of how a reference base is derived, before `just android-repack-apk`.

WHAT. `rename` copies the APK to OUT and rewrites the package string in exactly these entries, each with an exact
expected count (a count that differs is a refusal, nothing is written):

    AndroidManifest.xml               1 occurrence, UTF-16LE (the manifest's string pool), 0 ASCII
    lib/arm64-v8a/libr15.so           5 occurrences, ASCII, 0 UTF-16LE
    lib/arm64-v8a/libassetpatch.so    2 occurrences, ASCII, 0 UTF-16LE

The two names have the same length, so no offset, size or string-pool index moves: the replacement is byte for byte
in place, and the tool re-reads each result and checks its length and its remaining counts. The manifest is also
re-read as a string pool and compared string by string with the original. `META-INF/` is dropped (the base is
re-signed later by `just android-repack-apk`). Every other entry is copied with its bytes unchanged, in the original
order, with its original compression method, timestamp and attributes (zip extra fields, such as zipalign padding, are
not carried; the repack realigns).

The source package string is searched in every other entry, ASCII and UTF-16LE. A hit in an entry this list does not
name is a refusal that lists the entries; the tool never renames what the list does not name.

An input that is already renamed (the source string is nowhere, and the target string is in the three named entries at
least as often as the source was expected) is reported as `package_already_renamed` and copied, META-INF dropped, so a
second run over its own output writes the same entries again.

    quest_apk_rename_package.py rename --apk IN.apk --out OUT.apk [--from com.readyatdawn.afk] [--to com.readyatdawn.r15]

One JSON line on standard output per run (tag NEVR.QUEST.RENAME, the event, the counts, the sha256 in and out of
every changed entry); a refusal is one JSON line on standard error and exit status 1.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import sys
import zipfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import quest_apk_version as qv  # noqa: E402

TAG = "NEVR.QUEST.RENAME"
DEFAULT_FROM = "com.readyatdawn.afk"
DEFAULT_TO = "com.readyatdawn.r15"
MANIFEST = "AndroidManifest.xml"

# entry -> (expected ASCII occurrences, expected UTF-16LE occurrences) of the source package string.
EXPECTED = {
    MANIFEST: (0, 1),
    "lib/arm64-v8a/libr15.so": (5, 0),
    "lib/arm64-v8a/libassetpatch.so": (2, 0),
}


class RenameError(Exception):
    pass


def _sha(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def _forms(text: str) -> tuple:
    return text.encode("ascii"), text.encode("utf-16-le")


def _counts(data: bytes, text: str) -> tuple:
    ascii_form, wide_form = _forms(text)
    return data.count(ascii_form), data.count(wide_form)


def _pool_strings(data: bytes) -> list:
    """The manifest's string pool as text, UTF-16 pools only (the rename's count is of UTF-16LE bytes)."""
    try:
        kind, header_size, size = qv._chunk_header(data, 0, len(data))
        if kind != qv.RES_XML or header_size != 8 or size != len(data):
            raise RenameError("AndroidManifest.xml is not a compiled XML document")
        pos = 8
        while pos < len(data):
            kind, header_size, size = qv._chunk_header(data, pos, len(data))
            if kind == qv.RES_STRING_POOL:
                flags = int.from_bytes(data[pos + 16:pos + 20], "little")
                if flags & qv.UTF8_FLAG:
                    raise RenameError("the manifest's string pool is UTF-8; the rename counts UTF-16LE strings")
                return qv._string_pool(data, pos, header_size, size)
            pos += size
    except qv.ManifestError as error:
        raise RenameError(f"AndroidManifest.xml does not parse: {error}") from None
    raise RenameError("AndroidManifest.xml has no string pool")


def rename_manifest(data: bytes, old: str, new: str) -> bytes:
    """The manifest with `old` replaced by `new` in its UTF-16LE strings; the string pool must read back as the
    original with exactly that substitution, and every byte outside the replaced runs must be unchanged."""
    before = _pool_strings(data)
    old_wide, new_wide = old.encode("utf-16-le"), new.encode("utf-16-le")
    out = data.replace(old_wide, new_wide)
    after = _pool_strings(out)
    if len(out) != len(data) or after != [s.replace(old, new) for s in before]:
        raise RenameError("the renamed manifest's string pool is not the original with the substitution")
    return out


def rename_library(data: bytes, old: str, new: str) -> bytes:
    out = data.replace(old.encode("ascii"), new.encode("ascii"))
    if len(out) != len(data):
        raise RenameError("the renamed library changed length")
    return out


def _check_names(old: str, new: str) -> None:
    for name in (old, new):
        if not name or not name.isascii() or "\0" in name:
            raise RenameError(f"package name {name!r} must be non-empty ASCII")
    if len(old) != len(new):
        raise RenameError(f"{old!r} and {new!r} differ in length: the rename is in place and never moves a byte")
    if old == new:
        raise RenameError("the source and target package names are the same")


def _in_meta_inf(name: str) -> bool:
    return name.startswith("META-INF/") or name == "META-INF"


def rename_apk(src: Path, dst: Path, old: str = DEFAULT_FROM, new: str = DEFAULT_TO) -> dict:
    """Write `dst` from `src`; returns the record the CLI logs. RenameError (nothing written) unless every rule in the
    module docstring holds."""
    _check_names(old, new)
    if src.resolve() == dst.resolve():
        raise RenameError("the output is the input file: the tool never edits an APK in place")
    found: dict = {}          # entry -> (ascii, utf16) of `old`, for every non-META-INF entry that has a hit
    target_found: dict = {}   # entry -> (ascii, utf16) of `new` for the named entries
    names = []
    with zipfile.ZipFile(src) as zin:
        infos = zin.infolist()
        names = [i.filename for i in infos]
        if len(set(names)) != len(names):
            raise RenameError("the APK has duplicate entry names")
        for info in infos:
            if _in_meta_inf(info.filename) or info.is_dir():
                continue
            data = zin.read(info)
            hit = _counts(data, old)
            if any(hit):
                found[info.filename] = hit
            if info.filename in EXPECTED:
                target_found[info.filename] = _counts(data, new)
        missing = [n for n in EXPECTED if n not in names]
        if missing:
            raise RenameError(f"the APK has no entry {', '.join(missing)}")
        record = {"tag": TAG, "apk": src.name, "out": dst.name, "from": old, "to": new,
                  "dropped_meta_inf": sum(1 for n in names if _in_meta_inf(n))}
        unnamed = {n: c for n, c in found.items() if n not in EXPECTED}
        if unnamed:
            raise RenameError("the source package string is in entries the rename does not name (not renamed, "
                              "decide each): " + "; ".join(f"{n} ascii={a} utf16le={w}" for n, (a, w) in sorted(unnamed.items())))
        already = not found
        if already:
            for entry, (want_ascii, want_wide) in EXPECTED.items():
                have = target_found[entry]
                if have[0] < want_ascii or have[1] < want_wide:
                    raise RenameError(f"{entry} holds neither the source package string nor the target at least "
                                      f"{want_ascii} ASCII and {want_wide} UTF-16LE times (has ascii={have[0]} "
                                      f"utf16le={have[1]}): not a reference APK this tool knows")
            record.update(event="package_already_renamed", counts={e: list(target_found[e]) for e in EXPECTED}, changed={})
        else:
            for entry, expected in EXPECTED.items():
                if found.get(entry, (0, 0)) != expected:
                    raise RenameError(f"{entry} holds the source package string ascii/utf16le "
                                      f"{found.get(entry, (0, 0))}, expected {expected}; nothing written")
            record.update(event="package_renamed", counts={e: list(found[e]) for e in EXPECTED}, changed={})
        tmp = dst.with_name(dst.name + ".part")
        try:
            with zipfile.ZipFile(tmp, "w", allowZip64=True) as zout:
                for info in infos:
                    if _in_meta_inf(info.filename):
                        continue
                    data = zin.read(info)
                    if not already and info.filename in EXPECTED:
                        out = rename_manifest(data, old, new) if info.filename == MANIFEST else rename_library(data, old, new)
                        left = _counts(out, old)
                        gained = _counts(out, new)[0 if info.filename != MANIFEST else 1]
                        if left != (0, 0) or gained < sum(found[info.filename]):
                            raise RenameError(f"{info.filename}: the result still holds the source string, or lost the target")
                        record["changed"][info.filename] = {"sha256_in": _sha(data), "sha256_out": _sha(out)}
                        data = out
                    copy = zipfile.ZipInfo(info.filename, info.date_time)
                    copy.compress_type = info.compress_type
                    copy.external_attr = info.external_attr
                    copy.create_system = info.create_system
                    copy.comment = info.comment
                    zout.writestr(copy, data)
            os.replace(tmp, dst)
        finally:
            if tmp.exists():
                tmp.unlink()
    record["entries_out"] = len(names) - record["dropped_meta_inf"]
    record["sha256_out"] = hashlib.sha256(dst.read_bytes()).hexdigest()
    return record


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    sub = parser.add_subparsers(dest="cmd", required=True)
    rename = sub.add_parser("rename")
    rename.add_argument("--apk", type=Path, required=True)
    rename.add_argument("--out", type=Path, required=True)
    rename.add_argument("--from", dest="old", default=DEFAULT_FROM)
    rename.add_argument("--to", dest="new", default=DEFAULT_TO)
    args = parser.parse_args(argv)
    try:
        print(json.dumps(rename_apk(args.apk, args.out, args.old, args.new)))
        return 0
    except (OSError, zipfile.BadZipFile, RenameError) as error:
        print(json.dumps({"tag": TAG, "event": "package_rename_refused", "apk": args.apk.name, "reason": str(error)}),
              file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
