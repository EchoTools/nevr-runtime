#!/usr/bin/env python3
"""Make the combat content pack (docs/adr/0029-quest-bundled-content-pack.md) from the owner's game-data zip.

  python3 -I tools/quest_pack_from_game_data.py --game-data-zip ZIP --expected-sha256 HEX --out DIR [--header PATH]

The zip's whole-file sha-256 is checked against --expected-sha256 (the digest the signed release manifest lists for the
game-data artifact) before the zip is read. From it, the pack takes exactly two entries, the manifest the data step reads
and the one package the pack adds, and checks each against the pins in the header (src/quest/data/package_pins.h, default):
the manifest's digest against the data set's manifest digest, the package's length and digest against its pinned row. The
data-set manifest digest, the package rows, the required stock-manifest digest and the pack version are read from the
header's text; the data-folder version and manifest id are read from src/quest/data/plan.h (kManifestPath).

DIR receives pack.zip (the two entries, stored under their game-data paths) and pack_meta.json (id, version,
requires_stock_manifest_sha256, zip_prefix), the input of `just android-repack-apk ... pack=... pack_meta=...`.

Refusals exit 1 and name what failed. Nothing is written to DIR before every check has passed.
"""

import argparse
import hashlib
import json
import pathlib
import re
import sys
import zipfile

ROOT = pathlib.Path(__file__).resolve().parents[1]
DEFAULT_HEADER = ROOT / "src" / "quest" / "data" / "package_pins.h"
PLAN_HEADER = ROOT / "src" / "quest" / "data" / "plan.h"
PACK_ID = "combat-pack"


def refuse(message):
    print(f"quest-combat-pack: {message}", file=sys.stderr)
    sys.exit(1)


def read_pins(header_path):
    """The pins the header holds. Every value is parsed from the header text; none is written in this file."""
    if not header_path.is_file():
        refuse(f"pins header not found: {header_path}")
    text = header_path.read_text(encoding="utf-8")
    set_match = re.search(r'\{"([0-9a-f]{64})",\s*kCombatPackPackages', text)
    if not set_match:
        refuse(f"{header_path.name}: no kPinnedDataSets row names kCombatPackPackages")
    packages = {}
    for index, length, digest, _hashed in re.findall(
            r'\{(\d+),\s*(\d+)u,\s*"([0-9a-f]{64})",\s*(true|false)\}', text):
        packages[int(index)] = (int(length), digest)
    if 1 not in packages:
        refuse(f"{header_path.name}: kCombatPackPackages has no package 1 (the package the pack adds)")
    requires = re.search(r"requires the stock manifest\s+(?://\s*)?([0-9a-f]{64})", text)
    version = re.search(r"combatw19s (\d+\.\d+\.\d+)", text)
    if not requires or not version:
        refuse(f"{header_path.name}: the comment names no required stock manifest or no pack version")
    return {
        "manifest_sha256": set_match.group(1),
        "package_length": packages[1][0],
        "package_sha256": packages[1][1],
        "requires_stock_manifest_sha256": requires.group(1),
        "version": version.group(1),
    }


def read_layout():
    """The data-folder version and manifest id the data step reads (plan.h, kManifestPath)."""
    if not PLAN_HEADER.is_file():
        refuse(f"plan header not found: {PLAN_HEADER}")
    match = re.search(r'kManifestPath = "_data/(\d+)/rad15/android/manifests/([0-9a-f]{16})"', PLAN_HEADER.read_text(encoding="utf-8"))
    if not match:
        refuse(f"{PLAN_HEADER.name}: kManifestPath is not _data/<version>/rad15/android/manifests/<id>")
    return match.group(1), match.group(2)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.split("\n", 1)[0])
    parser.add_argument("--game-data-zip", required=True, type=pathlib.Path)
    parser.add_argument("--expected-sha256", required=True)
    parser.add_argument("--out", required=True, type=pathlib.Path)
    parser.add_argument("--header", type=pathlib.Path, default=DEFAULT_HEADER)
    args = parser.parse_args(argv)
    if not re.fullmatch(r"[0-9a-f]{64}", args.expected_sha256):
        refuse("--expected-sha256 is not 64 lowercase hex digits")
    if not args.game_data_zip.is_file():
        refuse(f"game-data zip not found: {args.game_data_zip}")
    data = args.game_data_zip.read_bytes()
    got = hashlib.sha256(data).hexdigest()
    if got != args.expected_sha256:
        refuse(f"game-data zip sha256 is {got}, the signed manifest lists {args.expected_sha256}")
    pins = read_pins(args.header)
    folder, manifest_id = read_layout()
    base = f"_data/{folder}/rad15/android/"
    manifest_name = base + "manifests/" + manifest_id
    package_name = base + "packages/" + manifest_id + "_1"
    try:
        with zipfile.ZipFile(args.game_data_zip) as zf:
            names = set(zf.namelist())
            for name in (manifest_name, package_name):
                if name not in names:
                    refuse(f"the game-data zip has no {name}")
            manifest = zf.read(manifest_name)
            package = zf.read(package_name)
    except zipfile.BadZipFile:
        refuse(f"the game-data zip is not a zip file: {args.game_data_zip}")
    if hashlib.sha256(manifest).hexdigest() != pins["manifest_sha256"]:
        refuse(f"{manifest_name} digest does not match the data set's pinned manifest digest")
    if len(package) != pins["package_length"]:
        refuse(f"{package_name} is {len(package)} bytes, the pinned length is {pins['package_length']}")
    if hashlib.sha256(package).hexdigest() != pins["package_sha256"]:
        refuse(f"{package_name} digest does not match its pinned digest")
    args.out.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(args.out / "pack.zip", "w", compression=zipfile.ZIP_STORED) as out:
        out.writestr(manifest_name, manifest)
        out.writestr(package_name, package)
    meta = {
        "id": PACK_ID,
        "requires_stock_manifest_sha256": [pins["requires_stock_manifest_sha256"]],
        "version": pins["version"],
        "zip_prefix": base,
    }
    (args.out / "pack_meta.json").write_text(json.dumps(meta, sort_keys=True) + "\n", encoding="utf-8")
    print(f"quest-combat-pack: pack {PACK_ID} {pins['version']} written to {args.out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
