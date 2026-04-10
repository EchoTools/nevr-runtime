#!/usr/bin/env python3
"""Generate override resource files for the combat mod.

Runs the echomod pipeline (extract → clone → modify), then copies modified
resource binaries to an _overrides/combat/ directory. Also generates a
manifest.json listing all resources with their type/name hashes.

For the 83 unmodified resources (hash-swapped copies), the manifest records
them with alias=true — the plugin redirects these lookups to the original
mpl_lobby_b_combat data at runtime, so no files are needed.

For the 3 modified resources (CActorData, CTransformCR, CGSceneResource),
the actual .bin files are written and the plugin loads them from disk.

Usage:
    python generate_resources.py --build-dir <echomod_build_dir> --output-dir <_overrides/combat>
"""

import argparse
import json
import logging
import os
import struct
import sys
from pathlib import Path

log = logging.getLogger("generate_resources")


from echomod.radhash import rad_hash


# Type hashes for resources that get data-modified by echomod
MODIFIED_TYPES = {
    "CActorDataResourceWin10":  rad_hash("CActorDataResourceWin10"),   # transforms repositioned
    "CTransformCRWin10":        rad_hash("CTransformCRWin10"),         # Y offset applied
}

# The arena scene is modified on the ARENA level, not the combat sublevel
ARENA_SCENE_TYPE = rad_hash("CGSceneResourceWin10")

COMBAT_SUBLEVEL = "mpl_arena_combat"
COMBAT_SUBLEVEL_HASH = rad_hash(COMBAT_SUBLEVEL)

ORIGINAL_COMBAT = "mpl_lobby_b_combat"
ORIGINAL_COMBAT_HASH = 0xCB9977F7FC2B4526

ARENA_LEVEL = "mpl_arena_a"
ARENA_LEVEL_HASH = rad_hash(ARENA_LEVEL)


def collect_resources(build_dir: Path, sublevel: str, arena_level: str) -> list[dict]:
    """Scan build directory for all resources, classifying as modified or alias."""
    resources = []
    sublevel_hash = rad_hash(sublevel)

    for type_dir in sorted(build_dir.iterdir()):
        if not type_dir.is_dir():
            continue
        type_name = type_dir.name
        type_hash = rad_hash(type_name)

        # Check combat sublevel resources
        bin_file = type_dir / f"{sublevel}.bin"
        if bin_file.is_file():
            is_modified = type_name in MODIFIED_TYPES
            resources.append({
                "type_name": type_name,
                "type_hash": type_hash,
                "name_hash": sublevel_hash,
                "level_name": sublevel,
                "bin_path": str(bin_file) if is_modified else None,
                "size": bin_file.stat().st_size if is_modified else 0,
                "is_modified": is_modified,
                "alias_to": ORIGINAL_COMBAT_HASH if not is_modified else None,
            })

        # Check arena scene modification
        arena_bin = type_dir / f"{arena_level}.bin"
        if arena_bin.is_file() and type_hash == ARENA_SCENE_TYPE:
            resources.append({
                "type_name": type_name,
                "type_hash": type_hash,
                "name_hash": rad_hash(arena_level),
                "level_name": arena_level,
                "bin_path": str(arena_bin),
                "size": arena_bin.stat().st_size,
                "is_modified": True,
                "alias_to": None,
            })

    return resources


def generate_overrides(resources: list[dict], output_dir: Path) -> dict:
    """Copy modified .bin files and generate manifest.json."""
    output_dir.mkdir(parents=True, exist_ok=True)

    manifest_entries = []
    total_file_size = 0
    modified_count = 0
    alias_count = 0

    for res in resources:
        entry = {
            "type_hash": f"0x{res['type_hash']:016x}",
            "name_hash": f"0x{res['name_hash']:016x}",
            "type_name": res["type_name"],
            "level_name": res["level_name"],
        }

        if res["is_modified"] and res["bin_path"]:
            # Copy the modified .bin file
            filename = f"0x{res['type_hash']:016x}.0x{res['name_hash']:016x}"
            src = Path(res["bin_path"])
            dst = output_dir / filename
            dst.write_bytes(src.read_bytes())

            entry["file"] = filename
            entry["size"] = res["size"]
            entry["alias"] = False
            total_file_size += res["size"]
            modified_count += 1
            log.info("  MODIFIED: %s/%s -> %s (%d bytes)",
                     res["type_name"], res["level_name"], filename, res["size"])
        else:
            # Alias — plugin redirects lookup to original hash at runtime
            entry["file"] = None
            entry["size"] = 0
            entry["alias"] = True
            entry["alias_to"] = f"0x{res['alias_to']:016x}"
            alias_count += 1

        manifest_entries.append(entry)

    # Write manifest
    manifest = {
        "version": 1,
        "sublevel": COMBAT_SUBLEVEL,
        "sublevel_hash": f"0x{COMBAT_SUBLEVEL_HASH:016x}",
        "original_combat_hash": f"0x{ORIGINAL_COMBAT_HASH:016x}",
        "arena_level": ARENA_LEVEL,
        "arena_hash": f"0x{ARENA_LEVEL_HASH:016x}",
        "resources": manifest_entries,
    }

    manifest_path = output_dir / "manifest.json"
    with open(manifest_path, "w") as f:
        json.dump(manifest, f, indent=2)

    log.info("")
    log.info("  %d modified resources (%d bytes on disk)", modified_count, total_file_size)
    log.info("  %d aliased resources (redirected at runtime, no files)", alias_count)
    log.info("  Manifest: %s", manifest_path)

    return manifest


def main():
    logging.basicConfig(level=logging.INFO, format="%(message)s")

    parser = argparse.ArgumentParser(
        description="Generate combat override files from echomod build output")
    parser.add_argument("--build-dir", type=Path, required=True,
                        help="Path to echomod build directory (after setuparchive phases 0-3)")
    parser.add_argument("--output-dir", type=Path, required=True,
                        help="Output directory for override files (e.g., _overrides/combat)")
    parser.add_argument("--sublevel", default=COMBAT_SUBLEVEL,
                        help=f"Combat sublevel name (default: {COMBAT_SUBLEVEL})")
    parser.add_argument("--arena", default=ARENA_LEVEL,
                        help=f"Arena level name (default: {ARENA_LEVEL})")

    args = parser.parse_args()

    if not args.build_dir.is_dir():
        log.error("Build directory does not exist: %s", args.build_dir)
        sys.exit(1)

    # Find the manifest subdirectory in the build dir
    mf_dirs = sorted(
        [d for d in args.build_dir.iterdir() if d.is_dir()],
        key=lambda d: sum(1 for _ in d.iterdir()),
        reverse=True,
    )
    if not mf_dirs:
        log.error("No directories found in %s", args.build_dir)
        sys.exit(1)
    build_mf = mf_dirs[0]

    log.info("Scanning build directory: %s", build_mf)
    resources = collect_resources(build_mf, args.sublevel, args.arena)

    if not resources:
        log.error("No resources found for sublevel '%s'", args.sublevel)
        sys.exit(1)

    log.info("Found %d total resources", len(resources))
    log.info("Generating overrides to %s", args.output_dir)
    generate_overrides(resources, args.output_dir)
    log.info("Done.")


if __name__ == "__main__":
    main()
