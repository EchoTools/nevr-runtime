#!/usr/bin/env python3
"""The library entries of the Quest repack: what a plugin may be called, and what must survive signing (#1117).

`just android-repack-apk` stages the sentinel as `lib/arm64-v8a/libovrplatformloader.so` and the store's own loader as
`libovrplatformloader_orig.so`, and, for a headset run of the plugin loader (docs/adr/0019-quest-plugin-loader.md),
one plugin `lib/arm64-v8a/<name>`. `zip -0 -X` replaces an entry that already has the path it is given, so a plugin
named like a game library would silently take that library's place and the signed APK would not boot. Two commands:

  check-plugin-name --apk STORE_APK --plugin PLUGIN_FILE
      Refuses (exit 1) a plugin whose file name is the file name of ANY `lib/` entry of the store APK (in any ABI
      directory) or of a library the recipe stages itself.

  verify-signed --source STORE_APK --signed SIGNED_APK --stage STAGE_DIR --patcher PATCHER [--extra-rows FILE]
                [--scratch DIR]
      Reads the SIGNED APK. Refuses (exit 1) unless every `lib/` entry of the store APK, except the two libraries the
      recipe replaces on purpose, is there with the same bytes, and every file the recipe staged under `lib/arm64-v8a/`
      is there with the staged bytes. The two are the loader (the shim takes its name) and libr15.so, which must equal
      what the patcher (quest_patch_libr15, docs/adr/0021 section 9) makes of the store APK's libr15.so: the tool runs
      here, on a copy of the store file in a temporary directory (under --scratch when given), with the same
      --extra-rows file the recipe gave it, and the sha-256 of the store file, of the patcher's output and of the signed
      entry are printed. A signed libr15 that is the store's bytes, a patcher run on another input, a missing entry, a
      patcher that fails or writes no file, and a patcher whose output is the store file are each a named problem.

  check-shim-rows --shim SHIM [--extra-rows FILE]
      Refuses (exit 1) a shim that was not built for the rows the library is patched with (docs/adr/0021 section 11). The
      patched libr15's build ID depends on the extra rows; a shim built for other rows reports `build_id_mismatch` and
      installs nothing, while the patched library's stubs branch through slots that only an attached shim fills, so the
      game would fault on the first patched call. With --extra-rows the shim must carry the file's sha-256 (the
      `kSha256` string the variant build embeds); without it the shim must not be a variant build (it must not carry the
      variant's `libr15_extra_rows` log event name).

  check-plugin-build-ids --apk STORE_APK --plugin PLUGIN_FILE
      Refuses (exit 1) a plugin the Quest loader would refuse for its build IDs (src/quest/plugin/plugin_image.h):
      one with no NEVR_DECLARE_SUPPORTED_BUILD_IDS note, or whose list does not hold the GNU build ID of the store
      APK's own `lib/arm64-v8a/libr15.so`, and a store libr15.so with no build ID. The notes are read the way the loader
      reads them: from the PT_NOTE segments, each name and description padded to 4 bytes, the IDs split on spaces, commas, tabs and line ends and matched whole.

All read zip entries with the standard library and name every entry they refuse.
"""
from __future__ import annotations

import argparse
import hashlib
import re
import struct
import pathlib
import subprocess
import sys
import tempfile
import zipfile

ABI_DIR = "lib/arm64-v8a"
# The libraries the recipe stages itself (the shim under the loader's name, and the store's loader renamed).
STAGED_BY_RECIPE = ("libovrplatformloader.so", "libovrplatformloader_orig.so")
# The one store entry the recipe replaces on purpose: the shim takes the loader's name.
REPLACED_ON_PURPOSE = f"{ABI_DIR}/libovrplatformloader.so"
# The other one: the patcher's output takes libr15's place (docs/adr/0021 section 9). It is compared with that output,
# never with the store's bytes.
PATCHED_LIBRARY = f"{ABI_DIR}/libr15.so"
# The patcher reads and writes one file of about 58 MB; a run that takes longer than this is a hang.
PATCHER_TIMEOUT_SECONDS = 600
_CHUNK = 1 << 20
LIBR15 = f"{ABI_DIR}/libr15.so"
PT_NOTE = 4
NT_GNU_BUILD_ID = 3
NEVR_NOTE_TYPE = 1


def lib_entries(archive: zipfile.ZipFile) -> list[str]:
    """Every file entry under `lib/`."""
    return [i.filename for i in archive.infolist() if i.filename.startswith("lib/") and not i.is_dir()]


def entry_digest(archive: zipfile.ZipFile, name: str) -> str:
    digest = hashlib.sha256()
    with archive.open(name) as stream:
        while chunk := stream.read(_CHUNK):
            digest.update(chunk)
    return digest.hexdigest()


def file_digest(path: pathlib.Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        while chunk := stream.read(_CHUNK):
            digest.update(chunk)
    return digest.hexdigest()


def plugin_name_collisions(store_entries: list[str], plugin_name: str) -> list[str]:
    """What `plugin_name` collides with: the `lib/` entries of the store APK and the libraries the recipe stages."""
    found = [entry for entry in store_entries if entry.rsplit("/", 1)[-1] == plugin_name]
    found += [f"{ABI_DIR}/{name} (staged by the recipe)" for name in STAGED_BY_RECIPE if name == plugin_name]
    return found


def check_plugin_name(apk: pathlib.Path, plugin: pathlib.Path) -> list[str]:
    with zipfile.ZipFile(apk) as archive:
        return plugin_name_collisions(lib_entries(archive), plugin.name)


# The event name only a variant build (NEVR_LIBR15_EXTRA_ROWS) compiles in: sentinel/libr15_identity.cpp logs it when the
# embedded rows are unparsable. A NUL-bounded string in the shim's read-only data.
VARIANT_MARKER = b"\0libr15_extra_rows\0"


def shim_rows_problems(shim: pathlib.Path, extra_rows: pathlib.Path | None) -> list[str]:
    """The reasons `shim` was not built for the rows the library is patched with; empty when it was."""
    try:
        data = shim.read_bytes()
    except OSError as error:
        return [f"{shim}: cannot be read: {error}"]
    if extra_rows is None:
        if VARIANT_MARKER in data:
            return [f"{shim.name} is a variant build (it carries extra rows) but no extra_rows file was given, so the library "
                    "would be patched without them and the shim would refuse it (build_id_mismatch)"]
        return []
    digest = file_digest(extra_rows)
    if digest.encode("ascii") + b"\0" not in data:
        return [f"{shim.name} was not built with {extra_rows.name} (sha-256 {digest}): rebuild it with "
                "-DNEVR_LIBR15_EXTRA_ROWS=<the same file>, else it refuses the patched library (build_id_mismatch)"]
    return []


def patched_digest(archive: zipfile.ZipFile, patcher: pathlib.Path, extra_rows: pathlib.Path | None,
                   scratch: pathlib.Path | None, digests: dict[str, str], problems: list[str]) -> str | None:
    """The sha-256 of the patcher's output for the store APK's libr15.so, or None with the reason in `problems`.

    The store file is copied out of the archive into a temporary directory that the `with` removes; the patcher is run on
    that copy and its output is read back from the file it was told to write. `digests` receives the store file's."""
    with tempfile.TemporaryDirectory(prefix="quest-repack-libr15-", dir=scratch) as tmp:
        work = pathlib.Path(tmp)
        stock, out, report = work / "stock-libr15.so", work / "patched-libr15.so", work / "report.json"
        with archive.open(PATCHED_LIBRARY) as source, stock.open("wb") as sink:
            while chunk := source.read(_CHUNK):
                sink.write(chunk)
        digests["store"] = file_digest(stock)
        command = [str(patcher.resolve()), "--stock", str(stock), "--out", str(out), "--report", str(report)]
        if extra_rows is not None:
            command += ["--extra-rows", str(extra_rows)]
        try:
            run = subprocess.run(command, capture_output=True, text=True, errors="replace", timeout=PATCHER_TIMEOUT_SECONDS)
        except (OSError, subprocess.TimeoutExpired) as error:
            problems.append(f"{PATCHED_LIBRARY}: the patcher {patcher} did not run to its end: {error}")
            return None
        if run.returncode != 0:
            problems.append(f"{PATCHED_LIBRARY}: the patcher {patcher} exited {run.returncode} on the store APK's libr15.so: "
                            f"{run.stderr.strip()[-500:]}")
            return None
        if not out.is_file() or out.stat().st_size == 0:
            problems.append(f"{PATCHED_LIBRARY}: the patcher {patcher} exited 0 and wrote no file")
            return None
        digests["patched"] = file_digest(out)
    return digests["patched"]


def verify_signed(source: pathlib.Path, signed: pathlib.Path, stage: pathlib.Path, patcher: pathlib.Path,
                  extra_rows: pathlib.Path | None = None, scratch: pathlib.Path | None = None) -> list[str]:
    """The problems found in `signed`; empty when every original library survived, libr15.so is the patcher's output
    for the store APK's and every staged one is there. The three digests of libr15.so are printed."""
    problems: list[str] = []
    with zipfile.ZipFile(source) as original, zipfile.ZipFile(signed) as result:
        present = set(result.namelist())
        for entry in lib_entries(original):
            if entry in (REPLACED_ON_PURPOSE, PATCHED_LIBRARY):
                continue
            if entry not in present:
                problems.append(f"{entry}: in the store APK, missing from the signed APK")
            elif entry_digest(original, entry) != entry_digest(result, entry):
                problems.append(f"{entry}: the signed APK holds different bytes than the store APK")
        digests: dict[str, str] = {}
        if PATCHED_LIBRARY not in original.namelist():
            problems.append(f"{PATCHED_LIBRARY}: missing from the store APK, so there is nothing to patch")
        else:
            patched = patched_digest(original, patcher, extra_rows, scratch, digests, problems)
            if PATCHED_LIBRARY not in present:
                problems.append(f"{PATCHED_LIBRARY}: the patcher's output belongs here, missing from the signed APK")
            else:
                digests["signed"] = entry_digest(result, PATCHED_LIBRARY)
                if patched is not None and digests["signed"] != patched:
                    problems.append(f"{PATCHED_LIBRARY}: the signed APK does not hold the patcher's output for the store "
                                    f"APK's libr15.so (signed sha-256 {digests['signed']}, patcher output {patched})")
            if patched is not None and patched == digests["store"]:
                problems.append(f"{PATCHED_LIBRARY}: the patcher's output is the store APK's libr15.so, byte for byte: "
                                "nothing was patched")
        for label in ("store", "patched", "signed"):
            print(f"quest_repack_libs: libr15.so sha256 {label:<7} {digests.get(label, '(not available)')}", flush=True)
        staged_dir = stage / ABI_DIR
        for staged in sorted(p for p in staged_dir.iterdir() if p.is_file()):
            entry = f"{ABI_DIR}/{staged.name}"
            if entry not in present:
                problems.append(f"{entry}: staged, missing from the signed APK")
            elif entry_digest(result, entry) != file_digest(staged):
                problems.append(f"{entry}: the signed APK holds different bytes than the staged file")
    return problems


def note_desc(image: bytes, owner: bytes, note_type: int) -> bytes | None:
    """The description of the first note owned by `owner` with `note_type` in a PT_NOTE segment of the 64-bit
    little-endian ELF `image`; None when there is none or the headers do not hold together (plugin_image.cpp)."""
    if len(image) < 64 or image[:4] != b"\x7fELF" or image[4] != 2 or image[5] != 1:
        return None
    phoff, = struct.unpack_from("<Q", image, 0x20)
    phentsize, phnum = struct.unpack_from("<HH", image, 0x36)
    if phentsize < 56 or phoff + phnum * phentsize > len(image):
        return None
    for k in range(phnum):
        p_type, _flags, p_offset, _vaddr, _paddr, p_filesz = struct.unpack_from("<IIQQQQ", image, phoff + k * phentsize)
        if p_type != PT_NOTE or p_offset + p_filesz > len(image):
            continue
        at, end = p_offset, p_offset + p_filesz
        while end - at >= 12:
            namesz, descsz, n_type = struct.unpack_from("<III", image, at)
            at += 12
            name_len, desc_len = (namesz + 3) & ~3, (descsz + 3) & ~3
            if name_len > end - at:
                break
            name = image[at:at + namesz]
            if n_type == note_type and name == owner and descsz <= end - at - name_len:
                return image[at + name_len:at + name_len + descsz]
            if desc_len > end - at - name_len:
                break
            at += name_len + desc_len
    return None


def build_id_problems(libr15: bytes, plugin: bytes, plugin_name: str) -> list[str]:
    """Why the loader would refuse `plugin` for its build IDs against this `libr15`; empty when it would not."""
    running = note_desc(libr15, b"GNU\0", NT_GNU_BUILD_ID)
    if not running:
        return [f"the store APK's {LIBR15} has no GNU build ID: nothing can be checked"]
    declared = note_desc(plugin, b"NEVR\0", NEVR_NOTE_TYPE)
    if declared is None:
        return [f"plugin {plugin_name} declares no libr15 build IDs (NEVR_DECLARE_SUPPORTED_BUILD_IDS); the loader "
                "refuses it as build_ids_undeclared"]
    ids = [i for i in re.split(r"[ ,\t\r\n]+", declared.split(b"\0", 1)[0].decode("ascii", "replace")) if i]
    if running.hex() not in ids:
        return [f"plugin {plugin_name} declares {ids or 'an empty list'}, not the store libr15 build ID {running.hex()}; "
                "the loader refuses it as build_id_unsupported"]
    return []


def check_plugin_build_ids(apk: pathlib.Path, plugin: pathlib.Path) -> list[str]:
    with zipfile.ZipFile(apk) as archive:
        if LIBR15 not in archive.namelist():
            return [f"the store APK has no {LIBR15}"]
        libr15 = archive.read(LIBR15)
    return build_id_problems(libr15, plugin.read_bytes(), plugin.name)


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest="command", required=True)
    name = sub.add_parser("check-plugin-name")
    name.add_argument("--apk", type=pathlib.Path, required=True)
    name.add_argument("--plugin", type=pathlib.Path, required=True)
    rows = sub.add_parser("check-shim-rows")
    rows.add_argument("--shim", type=pathlib.Path, required=True)
    rows.add_argument("--extra-rows", type=pathlib.Path, default=None)
    ids = sub.add_parser("check-plugin-build-ids")
    ids.add_argument("--apk", type=pathlib.Path, required=True)
    ids.add_argument("--plugin", type=pathlib.Path, required=True)
    verify = sub.add_parser("verify-signed")
    verify.add_argument("--source", type=pathlib.Path, required=True)
    verify.add_argument("--signed", type=pathlib.Path, required=True)
    verify.add_argument("--stage", type=pathlib.Path, required=True)
    verify.add_argument("--patcher", type=pathlib.Path, required=True)
    verify.add_argument("--extra-rows", type=pathlib.Path, default=None)
    verify.add_argument("--scratch", type=pathlib.Path, default=None)
    args = parser.parse_args(argv)
    if args.command == "check-plugin-name":
        problems = [f"plugin {args.plugin.name} would replace {c}" for c in check_plugin_name(args.apk, args.plugin)]
    elif args.command == "check-shim-rows":
        problems = shim_rows_problems(args.shim, args.extra_rows)
    elif args.command == "check-plugin-build-ids":
        problems = check_plugin_build_ids(args.apk, args.plugin)
    else:
        problems = verify_signed(args.source, args.signed, args.stage, args.patcher, args.extra_rows, args.scratch)
    for problem in problems:
        print(f"quest_repack_libs: {problem}", file=sys.stderr)
    if not problems:
        print(f"quest_repack_libs: {args.command} OK")
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
