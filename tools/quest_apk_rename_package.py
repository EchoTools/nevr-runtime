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
    lib/arm64-v8a/libvrapi.so         the count of its runtime in the known-runtimes table, ASCII, 0 UTF-16LE; a runtime the
                                      table does not name must hold 0

The two names have the same length, so no offset, size or string-pool index moves: the replacement is byte for byte
in place, and the tool re-reads each result and checks its length and its remaining counts. The manifest is also
re-read as a string pool and compared string by string with the original. `META-INF/` is dropped (the base is
re-signed later by `just android-repack-apk`). Every other entry is copied with its bytes unchanged, in the original
order, with its original compression method, timestamp and attributes (zip extra fields, such as zipalign padding, are
not carried; the repack realigns).

The source package string is searched in every other entry, ASCII and UTF-16LE. A hit in an entry this list does not
name is a refusal that lists the entries; the tool never renames what the list does not name.

An input that is already renamed (the source string is nowhere, and the target string is in the named entries at
least as often as the source was expected) is reported as `package_already_renamed` and copied, META-INF dropped, so a
second run over its own output writes the same entries again.

    quest_apk_rename_package.py rename --apk IN.apk --out OUT.apk [--from com.readyatdawn.afk] [--to com.readyatdawn.r15]
                                       [--label TEXT] [--known-runtimes FILE]

KNOWN RUNTIMES. The libvrapi of a reference build is looked up by the sha-256 of the library as the APK carries it (an
already-renamed library is looked up with the target put back to the source). `KNOWN_RUNTIMES` names the two reference
runtimes (the pinned reference APK's, 0 copies; the wave-2 base's, 1). A libvrapi the table does not name must hold the
source string 0 times, else the rename is refused: the tool never renames what its table does not name. `--known-runtimes
FILE` (and the `known_runtimes` argument of `rename_apk`) EXTENDS the built-in table, never replaces it: FILE is a JSON
object of sha-256 (64 hex digits) to [ASCII count, UTF-16LE count], two non-negative integers; an entry that names a
built-in runtime with a different count, and any other shape, is a refusal. Without the argument the table is the
built-in one. The argument exists so that a test can name its fixture library; it is not an environment variable.

LABEL. The reference build's launcher label reads "Echo Combat (port)"; the project's APK reads "Echo VR". With
`--label TEXT` the output's AndroidManifest.xml also has its `<application>` label replaced by TEXT, in place, through
`quest_apk_label.set_label`, after the package rename and in both the renamed and the already-renamed case. Nothing else
changes because of it: every other entry is byte for byte what it is without the flag. A label the editor refuses (longer
than the old one, referenced by another attribute, ...) is a refusal and nothing is written. Without the flag the
manifest's label is left as it is and the record has no label fields; with it the record carries `label_from` and
`label_to`.

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
import quest_apk_label as ql  # noqa: E402
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

# The reference runtime: its package-string count depends on the base it belongs to. A runtime in KNOWN_RUNTIMES (by the sha-256
# of the library as the APK carries it) must hold exactly its count; any other libvrapi must hold 0 (see KNOWN RUNTIMES above).
RUNTIME_ENTRY = "lib/arm64-v8a/libvrapi.so"
KNOWN_RUNTIMES = {
    # the 12-module runtime of the pinned reference APK (tools/quest_release_base.py REFERENCE_APK_SHA256): no package string
    "59a3cd2090b29f72cc14b853f1550073c9dd066c87c9cd120eed6d93f84c3839": (0, 0),
    # the 13-module wave-2 runtime: the package once, as the fallback name of a file-existence check
    "1212fcea7837111d7ae8ea835f64399f479a50cde89a0f44a003caedae12466a": (1, 0),
}
SHA256_HEX_DIGITS = 64


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


def _relabel(data: bytes, label: str, record: dict) -> bytes:
    """The manifest `data` with its launcher label set to `label`; the record gains `label_from` and `label_to`, and the
    manifest's `changed` entry names the final bytes. A label the editor refuses is a RenameError."""
    try:
        before = ql.label_of(data)
        out = ql.set_label(data, label)
        after = ql.label_of(out)
    except ql.LabelError as error:
        raise RenameError(f"{MANIFEST}: the launcher label was not set to {label!r}: {error}") from None
    record["label_from"], record["label_to"] = before, after
    entry = record["changed"].get(MANIFEST)
    if entry is not None:
        entry["sha256_out"] = _sha(out)
    elif out != data:
        record["changed"][MANIFEST] = {"sha256_in": _sha(data), "sha256_out": _sha(out)}
    return out


def _in_meta_inf(name: str) -> bool:
    return name.startswith("META-INF/") or name == "META-INF"


def merged_runtimes(extra: dict | None) -> dict:
    """The built-in KNOWN_RUNTIMES extended by `extra` (None or empty: the built-in table). An `extra` entry that names a
    built-in runtime with a different count is a RenameError."""
    merged = dict(KNOWN_RUNTIMES)
    for key, counts in (extra or {}).items():
        if key in KNOWN_RUNTIMES and tuple(KNOWN_RUNTIMES[key]) != tuple(counts):
            raise RenameError(f"known-runtimes: {key} is a built-in runtime with counts {KNOWN_RUNTIMES[key]}, not {tuple(counts)}")
        merged[key] = tuple(counts)
    return merged


def load_known_runtimes(path: Path) -> dict:
    """The `--known-runtimes` file: a JSON object of sha-256 (64 hex digits) to [ASCII count, UTF-16LE count]. Any other
    shape is a RenameError."""
    try:
        raw = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, ValueError) as error:
        raise RenameError(f"known-runtimes: {path.name} cannot be read as JSON: {error}") from None
    if not isinstance(raw, dict):
        raise RenameError(f"known-runtimes: {path.name} is not a JSON object of sha-256 to [ascii count, utf16le count]")
    table = {}
    for key, value in raw.items():
        if len(key) != SHA256_HEX_DIGITS or any(c not in "0123456789abcdefABCDEF" for c in key):
            raise RenameError(f"known-runtimes: key {key!r} is not {SHA256_HEX_DIGITS} hex digits")
        if (not isinstance(value, list) or len(value) != 2
                or any(isinstance(n, bool) or not isinstance(n, int) or n < 0 for n in value)):
            raise RenameError(f"known-runtimes: {key} must map to [ascii count, utf16le count], two non-negative integers")
        table[key.lower()] = (value[0], value[1])
    return merged_runtimes(table)


def _runtime_expected(data: bytes, old: str, new: str, renamed: bool, known: dict) -> tuple:
    """The (ASCII, UTF-16LE) count of `old` that the runtime in `data` must hold. A runtime in `known` has its table count
    (an already-renamed input is looked up as it was before the rename, `new` put back to `old`); any other holds none."""
    candidates = [data] + ([data.replace(new.encode("ascii"), old.encode("ascii"))] if renamed else [])
    for candidate in candidates:
        if _sha(candidate) in known:
            return known[_sha(candidate)]
    return (0, 0)


def rename_apk(src: Path, dst: Path, old: str = DEFAULT_FROM, new: str = DEFAULT_TO, label: str | None = None,
               known_runtimes: dict | None = None) -> dict:
    """Write `dst` from `src`; returns the record the CLI logs. RenameError (nothing written) unless every rule in the
    module docstring holds. A `label` also sets the manifest's launcher label (see LABEL in the module docstring).
    `known_runtimes` extends the built-in KNOWN_RUNTIMES (None: the built-in table; see KNOWN RUNTIMES)."""
    _check_names(old, new)
    known = merged_runtimes(known_runtimes)
    if src.resolve() == dst.resolve():
        raise RenameError("the output is the input file: the tool never edits an APK in place")
    found: dict = {}          # entry -> (ascii, utf16) of `old`, for every non-META-INF entry that has a hit
    target_found: dict = {}   # entry -> (ascii, utf16) of `new` for the named entries
    runtime = None
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
            if info.filename in EXPECTED or info.filename == RUNTIME_ENTRY:
                target_found[info.filename] = _counts(data, new)
            if info.filename == RUNTIME_ENTRY:
                runtime = data
        missing = [n for n in (*EXPECTED, RUNTIME_ENTRY) if n not in names]
        if missing:
            raise RenameError(f"the APK has no entry {', '.join(missing)}")
        expected_all = {**EXPECTED, RUNTIME_ENTRY: _runtime_expected(runtime, old, new, not found, known)}
        record = {"tag": TAG, "apk": src.name, "out": dst.name, "from": old, "to": new,
                  "dropped_meta_inf": sum(1 for n in names if _in_meta_inf(n))}
        unnamed = {n: c for n, c in found.items() if n not in expected_all}
        if unnamed:
            raise RenameError("the source package string is in entries the rename does not name (not renamed, "
                              "decide each): " + "; ".join(f"{n} ascii={a} utf16le={w}" for n, (a, w) in sorted(unnamed.items())))
        already = not found
        if already:
            for entry, (want_ascii, want_wide) in expected_all.items():
                have = target_found[entry]
                if have[0] < want_ascii or have[1] < want_wide:
                    raise RenameError(f"{entry} holds neither the source package string nor the target at least "
                                      f"{want_ascii} ASCII and {want_wide} UTF-16LE times (has ascii={have[0]} "
                                      f"utf16le={have[1]}): not a reference APK this tool knows")
            record.update(event="package_already_renamed", counts={e: list(target_found[e]) for e in expected_all}, changed={})
        else:
            for entry, expected in expected_all.items():
                if found.get(entry, (0, 0)) != expected:
                    raise RenameError(f"{entry} holds the source package string ascii/utf16le "
                                      f"{found.get(entry, (0, 0))}, expected {expected}; nothing written")
            record.update(event="package_renamed", counts={e: list(found.get(e, (0, 0))) for e in expected_all}, changed={})
        tmp = dst.with_name(dst.name + ".part")
        try:
            with zipfile.ZipFile(tmp, "w", allowZip64=True) as zout:
                for info in infos:
                    if _in_meta_inf(info.filename):
                        continue
                    data = zin.read(info)
                    if not already and info.filename in expected_all and expected_all[info.filename] != (0, 0):
                        out = rename_manifest(data, old, new) if info.filename == MANIFEST else rename_library(data, old, new)
                        left = _counts(out, old)
                        gained = _counts(out, new)[0 if info.filename != MANIFEST else 1]
                        if left != (0, 0) or gained < sum(found[info.filename]):
                            raise RenameError(f"{info.filename}: the result still holds the source string, or lost the target")
                        record["changed"][info.filename] = {"sha256_in": _sha(data), "sha256_out": _sha(out)}
                        data = out
                    if label is not None and info.filename == MANIFEST:
                        data = _relabel(data, label, record)
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
    rename.add_argument("--label", default=None, help="also set the manifest's launcher label to this text (in place, no longer than the old one)")
    rename.add_argument("--known-runtimes", type=Path, default=None, dest="known_runtimes",
                        help="JSON file naming more reference runtimes, sha-256 -> [ascii, utf16le]; extends the built-in table")
    args = parser.parse_args(argv)
    try:
        known = load_known_runtimes(args.known_runtimes) if args.known_runtimes else None
        print(json.dumps(rename_apk(args.apk, args.out, args.old, args.new, args.label, known)))
        return 0
    except (OSError, zipfile.BadZipFile, RenameError) as error:
        print(json.dumps({"tag": TAG, "event": "package_rename_refused", "apk": args.apk.name, "reason": str(error)}),
              file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
