"""Repack modified resources back into Echo VR archives."""

from __future__ import annotations
import json
import struct
from pathlib import Path

try:
    import zstandard as zstd
    HAS_ZSTD = True
except ImportError:
    HAS_ZSTD = False

from .manifest import Manifest, ResourceIndexEntry, FrameEntry, TableDescriptor
from .package import PackageReader, PackageWriter
from ..radhash import rad_hash


def repack_archive(
    game_data_dir: str,
    modified_dir: str,
    output_dir: str,
    manifest_name: str | None = None,
    hash_names_path: str | None = None,
    compression_level: int = 3,
) -> str:
    """Repack an archive with modified resources.

    Args:
        game_data_dir: Original game data dir (manifests/ and packages/)
        modified_dir: Directory with modified resource files (same layout as extracted)
        output_dir: Where to write the new archive
        manifest_name: Manifest filename (auto-detected if None)
        hash_names_path: Path to hash_names.json for name resolution
        compression_level: ZSTD compression level

    Returns:
        Path to the new manifest file.
    """
    if not HAS_ZSTD:
        raise ImportError("zstandard package required: pip install zstandard")

    base = Path(game_data_dir)
    manifests_dir = base / "manifests"
    packages_dir = base / "packages"
    mod_base = Path(modified_dir)
    out_base = Path(output_dir)

    # Auto-detect manifest
    if manifest_name is None:
        manifests = sorted(manifests_dir.iterdir(), key=lambda f: f.stat().st_size, reverse=True)
        manifest_name = manifests[0].name

    # Load original manifest
    manifest = Manifest.from_file(str(manifests_dir / manifest_name))

    # Build reverse hash lookup
    hash_db = {}
    if hash_names_path:
        with open(hash_names_path) as f:
            raw = json.load(f)
        for k, v in raw.items():
            try:
                hash_db[v] = int(k, 16)
                hash_db[int(k, 16)] = v
            except (ValueError, TypeError):
                pass

    # Find modified resources
    reader = PackageReader(manifest, str(packages_dir), manifest_name)
    writer = PackageWriter(manifest)

    for type_dir in mod_base.iterdir():
        if not type_dir.is_dir():
            continue
        type_name = type_dir.name
        type_hash = rad_hash(type_name)

        for res_file in type_dir.glob("*.bin"):
            res_name = res_file.stem
            # Try to resolve resource hash
            if res_name in hash_db:
                res_hash = hash_db[res_name]
            else:
                res_hash = rad_hash(res_name)

            with open(res_file, "rb") as f:
                data = f.read()

            writer.set_resource(type_hash, res_hash, data)

    # Repack
    out_packages = out_base / "packages"
    out_manifests = out_base / "manifests"
    out_packages.mkdir(parents=True, exist_ok=True)
    out_manifests.mkdir(parents=True, exist_ok=True)

    new_manifest = writer.repack(reader, str(out_packages), compression_level)

    # Write new manifest
    manifest_path = out_manifests / manifest_name
    _write_manifest(new_manifest, str(manifest_path))

    return str(manifest_path)


def _write_manifest(manifest: Manifest, path: str) -> None:
    """Serialize and ZSTD-compress a manifest to disk."""
    from ..binary_utils import BinaryWriter

    w = BinaryWriter()

    # Package count
    w.write_u64(manifest.package_count)

    # Update table descriptors
    desc0 = TableDescriptor(
        total_byte_size=len(manifest.resources) * 32,
        entry_stride=32, entry_count=len(manifest.resources),
        capacity=len(manifest.resources),
    )
    desc1 = TableDescriptor(
        total_byte_size=len(manifest.extended_info) * 40,
        entry_stride=40, entry_count=len(manifest.extended_info),
        capacity=len(manifest.extended_info),
    )
    desc2 = TableDescriptor(
        total_byte_size=len(manifest.frames) * 16,
        entry_stride=16, entry_count=len(manifest.frames),
        capacity=len(manifest.frames),
    )

    for desc in [desc0, desc1, desc2]:
        desc.to_writer(w)

    # Table 0: resources
    for r in manifest.resources:
        w.write_u64(r.type_hash)
        w.write_u64(r.resource_hash)
        w.write_u32(r.frame_index)
        w.write_u32(r.offset)
        w.write_u32(r.size)
        w.write_u32(r.package_index)

    # Table 1: extended info
    for e in manifest.extended_info:
        w.write_u64(e.type_hash)
        w.write_u64(e.resource_hash)
        w.write_bytes(e.extra_data)

    # Table 2: frames
    for f in manifest.frames:
        w.write_u32(f.package_path_index)
        w.write_u32(f.cumulative_offset)
        w.write_u32(f.compressed_size)
        w.write_u32(f.decompressed_size)

    decompressed = w.getvalue()

    # Compress with ZSTD
    cctx = zstd.ZstdCompressor(level=3)
    compressed = cctx.compress(decompressed)

    # Write with ZSTD wrapper header
    with open(path, "wb") as f:
        f.write(b"ZSTD")
        f.write(struct.pack("<I", 24))  # header size
        f.write(struct.pack("<Q", len(decompressed)))
        f.write(struct.pack("<Q", len(compressed)))
        f.write(compressed)
