"""CLI entry point for echomod toolkit.

Usage:
    python -m echomod extract <game_data_dir> <output_dir> [--hash-names <path>]
    python -m echomod clone-actor <extracted_dir> <level> <source_actor> <new_name> <x> <y> <z> [--output <dir>]
    python -m echomod repack <game_data_dir> <modified_dir> <output_dir> [--hash-names <path>]
    python -m echomod info <resource_file>
    python -m echomod hash <string>
"""

import argparse
import sys
import os


def cmd_extract(args):
    from .archive.extractor import extract_archive
    count = extract_archive(
        args.game_data_dir, args.output_dir,
        hash_names_path=args.hash_names,
        manifest_name=args.manifest,
    )
    print(f"Extracted {count} resources to {args.output_dir}")


def cmd_clone_actor(args):
    from .operations.actor_clone import clone_actor_by_name, write_modified_files
    modified = clone_actor_by_name(
        args.extracted_dir, args.level,
        args.source_actor, args.new_name,
        position=(args.x, args.y, args.z),
    )
    output = args.output or args.extracted_dir
    written = write_modified_files(modified, output)
    print(f"Modified {len(written)} files:")
    for f in written:
        print(f"  {f}")


def cmd_repack(args):
    from .archive.repacker import repack_archive
    manifest_path = repack_archive(
        args.game_data_dir, args.modified_dir, args.output_dir,
        hash_names_path=args.hash_names,
        compression_level=args.compression,
    )
    print(f"Repacked archive. New manifest: {manifest_path}")


def cmd_info(args):
    from .descriptors import CRDescriptor
    from .binary_utils import BinaryReader

    with open(args.resource_file, "rb") as f:
        data = f.read()

    print(f"File: {args.resource_file}")
    print(f"Size: {len(data)} bytes")

    if len(data) >= 56:
        r = BinaryReader(data)
        desc = CRDescriptor.from_reader(r)
        if desc.count > 0 and desc.data_size > 0:
            entry_size = desc.data_size // desc.count
            print(f"CRDescriptor: count={desc.count}, capacity={desc.capacity}, "
                  f"data_size={desc.data_size}, entry_size={entry_size}, flags={desc.flags}")
            expected = 56 + desc.data_size
            tail = len(data) - expected
            if tail > 0:
                print(f"Trailing data: {tail} bytes (compound CR)")
            elif tail == 0:
                print("Simple CR (no trailing data)")
        else:
            print("Not a standard CR file (count=0 or data_size=0)")


def cmd_hash(args):
    from .radhash import rad_hash
    h = rad_hash(args.string)
    print(f"'{args.string}' -> 0x{h:016X}")


def main():
    parser = argparse.ArgumentParser(prog="echomod", description="Echo VR modding toolkit")
    sub = parser.add_subparsers(dest="command", required=True)

    # extract
    p = sub.add_parser("extract", help="Extract resources from game archives")
    p.add_argument("game_data_dir", help="Path containing manifests/ and packages/")
    p.add_argument("output_dir", help="Output directory for extracted files")
    p.add_argument("--hash-names", help="Path to hash_names.json")
    p.add_argument("--manifest", help="Manifest filename (auto-detected if omitted)")
    p.set_defaults(func=cmd_extract)

    # clone-actor
    p = sub.add_parser("clone-actor", help="Clone an actor in a level")
    p.add_argument("extracted_dir", help="Path to extracted game files")
    p.add_argument("level", help="Level name (e.g., mpl_arena_a)")
    p.add_argument("source_actor", help="Source actor name to clone")
    p.add_argument("new_name", help="Name for the cloned actor")
    p.add_argument("x", type=float, help="X position")
    p.add_argument("y", type=float, help="Y position")
    p.add_argument("z", type=float, help="Z position")
    p.add_argument("--output", help="Output dir (defaults to extracted_dir)")
    p.set_defaults(func=cmd_clone_actor)

    # repack
    p = sub.add_parser("repack", help="Repack modified resources into archives")
    p.add_argument("game_data_dir", help="Original game data dir")
    p.add_argument("modified_dir", help="Directory with modified resources")
    p.add_argument("output_dir", help="Output directory for new archive")
    p.add_argument("--hash-names", help="Path to hash_names.json")
    p.add_argument("--compression", type=int, default=3, help="ZSTD compression level")
    p.set_defaults(func=cmd_repack)

    # info
    p = sub.add_parser("info", help="Show info about a resource file")
    p.add_argument("resource_file", help="Path to a .bin resource file")
    p.set_defaults(func=cmd_info)

    # hash
    p = sub.add_parser("hash", help="Compute RAD CRC-64 hash of a string")
    p.add_argument("string", help="String to hash")
    p.set_defaults(func=cmd_hash)

    args = parser.parse_args()
    args.func(args)


if __name__ == "__main__":
    main()
