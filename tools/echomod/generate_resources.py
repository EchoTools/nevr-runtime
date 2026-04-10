#!/usr/bin/env python3
"""Generate embedded C headers from echomod resource binaries.

Runs the echomod pipeline (extract → clone → modify), then converts each
modified resource .bin file to a C header with a const uint8_t[] array.
Also generates a master combat_resources.h with a registry table.

Usage:
    python generate_resources.py --data-dir <echovr/_data> --output-dir <plugins/combat-mod/generated>
    python generate_resources.py --build-dir <existing_build_dir> --output-dir <plugins/combat-mod/generated>
"""

import argparse
import json
import logging
import os
import struct
import sys
from pathlib import Path

log = logging.getLogger("generate_resources")

# Hash lookup for type names → type hashes (from echomod/archive/manifest.py)
# These are the CR resource type hashes used in the manifest.
TYPE_HASHES = {
    "CActorDataResourceWin10":    0x9C3FF8DA1D5F6D50,
    "CGSceneResourceWin10":       0x93CE2B7797D6FDE4,
    "CTransformCRWin10":          0x72A3DAB4F43BB3E4,
    "CModelCRWin10":              0x7C42C1091EA50BD2,
    "CAnimationCRWin10":          0xCBD30F590E2DF49B,
    "CScriptCRWin10":             0x36A2DCF3F1D3F9AA,
    "CComponentLODCRWin10":       0x59B1A3CFB5A1F681,
    "CParticleEffectCRWin10":     0x6D5C64A6C35F1F7A,
    "CR15SyncGrabCRWin10":        0xA8D68A1A87E9BA80,
    "CInstanceModelCRWin10":      0xBB227D69B1A1BE08,
    "CArchiveResourceWin10":      0x25C98BFC6C91CF0D,
    # Catch-all for unknown types — derive from directory name via radhash
}


def rad_hash(name: str) -> int:
    """CRC-64 hash matching the RAD engine (case-insensitive)."""
    poly = 0x95AC9329AC4BC9B5
    h = 0xFFFFFFFFFFFFFFFF
    for c in name.lower():
        h ^= ord(c)
        for _ in range(8):
            if h & 1:
                h = (h >> 1) ^ poly
            else:
                h >>= 1
    return h ^ 0xFFFFFFFFFFFFFFFF


def bin_to_header(bin_path: Path, var_name: str) -> str:
    """Convert a binary file to a C header with a const uint8_t[] array."""
    data = bin_path.read_bytes()
    lines = [
        f"/* Auto-generated from {bin_path.name} — do not edit */",
        "#pragma once",
        "#include <cstddef>",
        "#include <cstdint>",
        "",
        f"static const uint8_t {var_name}[] = {{",
    ]

    # Format 16 bytes per line
    for i in range(0, len(data), 16):
        chunk = data[i:i+16]
        hex_str = ", ".join(f"0x{b:02x}" for b in chunk)
        lines.append(f"    {hex_str},")

    lines.append("};")
    lines.append(f"static const size_t {var_name}_len = sizeof({var_name});")
    lines.append("")
    return "\n".join(lines)


def sanitize_name(name: str) -> str:
    """Convert a resource name to a valid C identifier."""
    return name.replace("-", "_").replace(".", "_").replace(" ", "_")


def collect_modified_resources(build_dir: Path, level_name: str) -> list[dict]:
    """Scan build directory for modified resource files for the given level."""
    resources = []

    for type_dir in sorted(build_dir.iterdir()):
        if not type_dir.is_dir():
            continue
        type_name = type_dir.name

        # Look for the target level's resource file
        bin_file = type_dir / f"{level_name}.bin"
        if not bin_file.is_file():
            continue

        # Resolve type hash
        type_hash = TYPE_HASHES.get(type_name)
        if type_hash is None:
            type_hash = rad_hash(type_name)

        name_hash = rad_hash(level_name)

        resources.append({
            "type_name": type_name,
            "type_hash": type_hash,
            "name_hash": name_hash,
            "level_name": level_name,
            "bin_path": bin_file,
            "size": bin_file.stat().st_size,
            "is_new": True,  # Sublevel resources are new (not in original _data)
        })

    return resources


def generate_headers(resources: list[dict], output_dir: Path) -> Path:
    """Generate C headers for each resource and a master registry header."""
    output_dir.mkdir(parents=True, exist_ok=True)

    entries = []
    for res in resources:
        var_name = f"kResource_{sanitize_name(res['type_name'])}_{sanitize_name(res['level_name'])}"
        header_name = f"{res['type_name']}_{res['level_name']}.h"

        header_content = bin_to_header(res["bin_path"], var_name)
        header_path = output_dir / header_name
        header_path.write_text(header_content)

        entries.append({
            "header": header_name,
            "var_name": var_name,
            "type_hash": res["type_hash"],
            "name_hash": res["name_hash"],
            "is_new": res["is_new"],
            "label": f"{res['type_name']}/{res['level_name']}",
            "size": res["size"],
        })

        log.info("  Generated %s (%d bytes)", header_name, res["size"])

    # Generate master header
    master_lines = [
        "/* Auto-generated combat resource registry — do not edit */",
        "#pragma once",
        "#include <cstddef>",
        "#include <cstdint>",
        "",
    ]

    for entry in entries:
        master_lines.append(f'#include "{entry["header"]}"')

    master_lines.append("")
    master_lines.append("struct EmbeddedResource {")
    master_lines.append("    uint64_t type_hash;")
    master_lines.append("    uint64_t name_hash;")
    master_lines.append("    const uint8_t* data;")
    master_lines.append("    size_t size;")
    master_lines.append("    bool is_new;")
    master_lines.append("    const char* label;")
    master_lines.append("};")
    master_lines.append("")
    master_lines.append(f"static const EmbeddedResource kCombatResources[] = {{")

    for entry in entries:
        master_lines.append(
            f"    {{0x{entry['type_hash']:016x}ULL, 0x{entry['name_hash']:016x}ULL, "
            f"{entry['var_name']}, {entry['var_name']}_len, "
            f"{'true' if entry['is_new'] else 'false'}, "
            f'"{entry["label"]}"}},')

    master_lines.append("};")
    master_lines.append(f"static const size_t kCombatResourceCount = sizeof(kCombatResources) / sizeof(kCombatResources[0]);")
    master_lines.append("")

    master_path = output_dir / "combat_resources.h"
    master_path.write_text("\n".join(master_lines))
    log.info("  Generated combat_resources.h (%d resources)", len(entries))

    return master_path


def main():
    logging.basicConfig(level=logging.INFO, format="%(message)s")

    parser = argparse.ArgumentParser(
        description="Generate embedded C headers from echomod combat resources")
    parser.add_argument("--data-dir", type=Path,
                        help="Path to echovr/_data (runs full extract+clone pipeline)")
    parser.add_argument("--build-dir", type=Path,
                        help="Path to existing echomod build directory (skip extract+clone)")
    parser.add_argument("--output-dir", type=Path, required=True,
                        help="Output directory for generated headers")
    parser.add_argument("--level", default="mpl_arena_combat",
                        help="Target level name (default: mpl_arena_combat)")

    args = parser.parse_args()

    if args.build_dir:
        build_dir = args.build_dir
        if not build_dir.is_dir():
            log.error("Build directory does not exist: %s", build_dir)
            sys.exit(1)
    elif args.data_dir:
        log.error("--data-dir mode (full pipeline) not yet implemented. "
                   "Run setuparchive.py first, then use --build-dir.")
        sys.exit(1)
    else:
        log.error("Specify either --data-dir or --build-dir")
        sys.exit(1)

    log.info("Collecting resources for level '%s' from %s", args.level, build_dir)
    resources = collect_modified_resources(build_dir, args.level)

    if not resources:
        log.error("No resources found for level '%s' in %s", args.level, build_dir)
        sys.exit(1)

    total_size = sum(r["size"] for r in resources)
    log.info("Found %d resources (%d bytes total)", len(resources), total_size)

    if total_size > 2 * 1024 * 1024:
        log.warning("Total embedded size exceeds 2MB (%d bytes). Consider compression.",
                     total_size)

    log.info("Generating C headers to %s", args.output_dir)
    master = generate_headers(resources, args.output_dir)
    log.info("Done. Master header: %s", master)


if __name__ == "__main__":
    main()
