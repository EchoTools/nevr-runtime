#!/usr/bin/env python3
"""Compile the combat script libraries from a libsrc directory the owner supplies (`just build-quest-combat-libs`).

  python3 -I tools/build_quest_combat_libs.py --libsrc DIR --store-apk APK --out DIR \
      --ndk-clang PATH --llvm-bin DIR --check-script PATH

DIR holds LIBS.json and one directory per library, <hash16>/ with <hash16>.cpp and, when the link row says so,
<hash16>.donor.s. The table is LIBS.json's "libs" object: each key is the library's file name, lib<hash16>.so, and each
row has "link" with the booleans "asm" (add the donor object), "libr15" (link the stock libr15.so) and "ldl" (add -ldl).
Nothing from DIR is copied into the repository, and no library is built unless DIR names it.

Each library is compiled with the NDK clang the recipe names:
  <ndk-clang> -shared -fPIC -O2 -nostdlib++ -I<DIR>/include -Wl,--gc-sections -Wl,--allow-shlib-undefined <hash16>.cpp
              [<hash16>.donor.s] [-L<out>/stock -l:libr15.so] [-ldl] -o <out>/work/<hash16>/lib<hash16>.so
The stock libr15.so is taken from the store APK. Each output then runs `<check-script> --plugin <llvm-bin> <file>`
(tools/check_quest_static_init.sh) and is written to <out>/lib<hash16>.so with its sha256 in <out>/SHA256SUMS.

Refusals exit 1 and name what is wrong: no LIBS.json, a key that is not lib<16 lowercase hex>.so, a missing source, a
missing link row, a store APK without libr15.so, a compiler or static-init failure. Every compiler run keeps its command
and output in <out>/work/<hash16>/build.log.
"""

import argparse
import hashlib
import json
import pathlib
import re
import subprocess
import sys
import zipfile

NAME = re.compile(r"^lib([0-9a-f]{16})\.so$")
LINK_KEYS = ("asm", "libr15", "ldl")
STOCK_MEMBER = "lib/arm64-v8a/libr15.so"


def refuse(message):
    print(f"build-quest-combat-libs: {message}", file=sys.stderr)
    sys.exit(1)


def load_plan(libsrc):
    path = libsrc / "LIBS.json"
    if not path.is_file():
        refuse(f"no LIBS.json in {libsrc} (the libsrc directory names the libraries to build)")
    try:
        plan = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, ValueError) as err:
        refuse(f"LIBS.json in {libsrc} does not parse: {err}")
    libs = plan.get("libs") if isinstance(plan, dict) else None
    if not isinstance(libs, dict) or not libs:
        refuse(f"LIBS.json in {libsrc} has no non-empty \"libs\" object")
    rows = []
    for name, row in sorted(libs.items()):
        match = NAME.match(name)
        if not match:
            refuse(f"LIBS.json key {name!r} is not named lib<hash16>.so (16 lowercase hex digits)")
        link = row.get("link") if isinstance(row, dict) else None
        if not isinstance(link, dict) or any(not isinstance(link.get(k, False), bool) for k in LINK_KEYS):
            refuse(f"LIBS.json row {name} has no \"link\" object of booleans asm, libr15 and ldl")
        cpp = libsrc / match.group(1) / f"{match.group(1)}.cpp"
        if not cpp.is_file():
            refuse(f"source of {name} is missing: {cpp}")
        rows.append((name, match.group(1), cpp, link))
    return rows


def extract_stock(store_apk, out):
    if not store_apk.is_file():
        refuse(f"store APK not found: {store_apk}")
    try:
        with zipfile.ZipFile(store_apk) as zf:
            data = zf.read(STOCK_MEMBER)
    except KeyError:
        refuse(f"the store APK has no {STOCK_MEMBER}")
    except zipfile.BadZipFile:
        refuse(f"the store APK is not a zip file: {store_apk}")
    stock = out / "stock"
    stock.mkdir(parents=True, exist_ok=True)
    (stock / "libr15.so").write_bytes(data)
    return stock


def compile_one(args, name, digits, cpp, link, libsrc, stock, out):
    work = out / "work" / digits
    work.mkdir(parents=True, exist_ok=True)
    so = work / name
    cmd = [args.ndk_clang, "-shared", "-fPIC", "-O2", "-nostdlib++", f"-I{libsrc / 'include'}",
           "-Wl,--gc-sections", "-Wl,--allow-shlib-undefined", str(cpp)]
    donor = libsrc / digits / f"{digits}.donor.s"
    if link["asm"]:
        if not donor.is_file():
            refuse(f"{name} links its donor object, and {donor} is missing")
        cmd.append(str(donor))
    if link["libr15"]:
        cmd += [f"-L{stock}", "-l:libr15.so"]
    if link["ldl"]:
        cmd.append("-ldl")
    cmd += ["-o", str(so)]
    run = subprocess.run(cmd, capture_output=True, text=True)
    (work / "build.log").write_text(" ".join(cmd) + "\n" + run.stdout + run.stderr, encoding="utf-8")
    if run.returncode != 0 or not so.is_file():
        refuse(f"compiling {name} failed (exit {run.returncode}); its command and output are in {work / 'build.log'}")
    check = subprocess.run([args.check_script, "--plugin", args.llvm_bin, str(so)], capture_output=True, text=True)
    if check.returncode != 0:
        refuse(f"{name} failed the static-init check (exit {check.returncode}): {check.stderr.strip() or check.stdout.strip()}")
    return so


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.split("\n", 1)[0])
    parser.add_argument("--libsrc", required=True, type=pathlib.Path)
    parser.add_argument("--store-apk", required=True, type=pathlib.Path)
    parser.add_argument("--out", required=True, type=pathlib.Path)
    parser.add_argument("--ndk-clang", required=True)
    parser.add_argument("--llvm-bin", required=True)
    parser.add_argument("--check-script", required=True)
    args = parser.parse_args(argv)
    libsrc = args.libsrc
    if not libsrc.is_dir():
        refuse(f"libsrc is not a directory: {libsrc}")
    rows = load_plan(libsrc)
    out = args.out
    out.mkdir(parents=True, exist_ok=True)
    stock = extract_stock(args.store_apk, out)
    sums = []
    for name, digits, cpp, link in rows:
        so = compile_one(args, name, digits, cpp, link, libsrc, stock, out)
        target = out / name
        target.write_bytes(so.read_bytes())
        sums.append(f"{hashlib.sha256(target.read_bytes()).hexdigest()}  {name}\n")
    (out / "SHA256SUMS").write_text("".join(sums), encoding="utf-8")
    print(f"build-quest-combat-libs: {len(rows)} libraries in {out}; SHA256SUMS written")
    return 0


if __name__ == "__main__":
    sys.exit(main())
