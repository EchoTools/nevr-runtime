#!/usr/bin/env python3
"""The library entries of the Quest repack: what a plugin may be called, and what must survive signing (#1117).

`just android-repack-apk` stages the sentinel as `lib/arm64-v8a/libovrplatformloader.so` and the store's own loader as
`libovrplatformloader_orig.so`, and, for a headset run of the plugin loader (docs/adr/0019-quest-plugin-loader.md),
one plugin `lib/arm64-v8a/<name>`. `zip -0 -X` replaces an entry that already has the path it is given, so a plugin
named like a game library would silently take that library's place and the signed APK would not boot. Two commands:

  check-plugin-name --apk STORE_APK --plugin PLUGIN_FILE
      Refuses (exit 1) a plugin whose file name is the file name of ANY `lib/` entry of the store APK (in any ABI
      directory) or of a library the recipe stages itself.

  verify-signed --source STORE_APK --signed SIGNED_APK --stage STAGE_DIR
      Reads the SIGNED APK. Refuses (exit 1) unless every `lib/` entry of the store APK, except the one library the
      recipe replaces on purpose (the loader), is there with the same bytes, and every file the recipe staged under
      `lib/arm64-v8a/` is there with the staged bytes.

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
import sys
import zipfile

ABI_DIR = "lib/arm64-v8a"
# The libraries the recipe stages itself (the shim under the loader's name, and the store's loader renamed).
STAGED_BY_RECIPE = ("libovrplatformloader.so", "libovrplatformloader_orig.so")
# The one store entry the recipe replaces on purpose: the shim takes the loader's name.
REPLACED_ON_PURPOSE = f"{ABI_DIR}/libovrplatformloader.so"
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


def verify_signed(source: pathlib.Path, signed: pathlib.Path, stage: pathlib.Path) -> list[str]:
    """The problems found in `signed`; empty when every original library survived and every staged one is there."""
    problems: list[str] = []
    with zipfile.ZipFile(source) as original, zipfile.ZipFile(signed) as result:
        present = set(result.namelist())
        for entry in lib_entries(original):
            if entry == REPLACED_ON_PURPOSE:
                continue
            if entry not in present:
                problems.append(f"{entry}: in the store APK, missing from the signed APK")
            elif entry_digest(original, entry) != entry_digest(result, entry):
                problems.append(f"{entry}: the signed APK holds different bytes than the store APK")
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
    ids = sub.add_parser("check-plugin-build-ids")
    ids.add_argument("--apk", type=pathlib.Path, required=True)
    ids.add_argument("--plugin", type=pathlib.Path, required=True)
    verify = sub.add_parser("verify-signed")
    verify.add_argument("--source", type=pathlib.Path, required=True)
    verify.add_argument("--signed", type=pathlib.Path, required=True)
    verify.add_argument("--stage", type=pathlib.Path, required=True)
    args = parser.parse_args(argv)
    if args.command == "check-plugin-name":
        problems = [f"plugin {args.plugin.name} would replace {c}" for c in check_plugin_name(args.apk, args.plugin)]
    elif args.command == "check-plugin-build-ids":
        problems = check_plugin_build_ids(args.apk, args.plugin)
    else:
        problems = verify_signed(args.source, args.signed, args.stage)
    for problem in problems:
        print(f"quest_repack_libs: {problem}", file=sys.stderr)
    if not problems:
        print(f"quest_repack_libs: {args.command} OK")
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
