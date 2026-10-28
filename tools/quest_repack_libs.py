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

Both read zip entries with the standard library and name every entry they refuse.
"""
from __future__ import annotations

import argparse
import hashlib
import pathlib
import sys
import zipfile

ABI_DIR = "lib/arm64-v8a"
# The libraries the recipe stages itself (the shim under the loader's name, and the store's loader renamed).
STAGED_BY_RECIPE = ("libovrplatformloader.so", "libovrplatformloader_orig.so")
# The one store entry the recipe replaces on purpose: the shim takes the loader's name.
REPLACED_ON_PURPOSE = f"{ABI_DIR}/libovrplatformloader.so"
_CHUNK = 1 << 20


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


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest="command", required=True)
    name = sub.add_parser("check-plugin-name")
    name.add_argument("--apk", type=pathlib.Path, required=True)
    name.add_argument("--plugin", type=pathlib.Path, required=True)
    verify = sub.add_parser("verify-signed")
    verify.add_argument("--source", type=pathlib.Path, required=True)
    verify.add_argument("--signed", type=pathlib.Path, required=True)
    verify.add_argument("--stage", type=pathlib.Path, required=True)
    args = parser.parse_args(argv)
    if args.command == "check-plugin-name":
        problems = [f"plugin {args.plugin.name} would replace {c}" for c in check_plugin_name(args.apk, args.plugin)]
    else:
        problems = verify_signed(args.source, args.signed, args.stage)
    for problem in problems:
        print(f"quest_repack_libs: {problem}", file=sys.stderr)
    if not problems:
        print(f"quest_repack_libs: {args.command} OK")
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
