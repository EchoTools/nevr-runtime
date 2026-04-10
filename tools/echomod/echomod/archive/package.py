"""Echo VR package reader/writer.

Package files are concatenated ZSTD frames. Each frame contains one or more
resources at specific offsets. The manifest maps resource hashes to frames.
"""

from __future__ import annotations
import os
from pathlib import Path

try:
    import zstandard as zstd
    HAS_ZSTD = True
except ImportError:
    HAS_ZSTD = False

from .manifest import Manifest, FrameEntry


class PackageReader:
    """Reads resources from Echo VR package files."""

    def __init__(self, manifest: Manifest, packages_dir: str, manifest_name: str):
        self.manifest = manifest
        self.packages_dir = Path(packages_dir)
        self.manifest_name = manifest_name
        self._frame_cache: dict[int, bytes] = {}

    def _get_package_path(self, index: int) -> Path:
        return self.packages_dir / f"{self.manifest_name}_{index}"

    def _read_frame(self, frame_index: int) -> bytes:
        """Read and decompress a single frame."""
        if frame_index in self._frame_cache:
            return self._frame_cache[frame_index]

        if not HAS_ZSTD:
            raise ImportError("zstandard package required: pip install zstandard")

        frame = self.manifest.frames[frame_index]

        # Determine which package file contains this offset
        pkg_files = sorted(self.packages_dir.glob(f"{self.manifest_name}_*"))
        cumulative = 0
        target_offset = frame.cumulative_offset

        for pkg_path in pkg_files:
            pkg_size = pkg_path.stat().st_size
            if cumulative + pkg_size > target_offset:
                local_offset = target_offset - cumulative
                with open(pkg_path, "rb") as f:
                    f.seek(local_offset)
                    compressed = f.read(frame.compressed_size)
                break
            cumulative += pkg_size
        else:
            raise ValueError(f"Frame {frame_index} offset {target_offset} beyond package files")

        dctx = zstd.ZstdDecompressor()
        decompressed = dctx.decompress(compressed, max_output_size=frame.decompressed_size + 1024)
        self._frame_cache[frame_index] = decompressed
        return decompressed

    def read_resource(self, type_hash: int, resource_hash: int) -> bytes | None:
        """Read a single resource by its type and resource hash."""
        entry = self.manifest.find_resource(type_hash, resource_hash)
        if entry is None:
            return None

        frame_data = self._read_frame(entry.frame_index)
        return frame_data[entry.offset:entry.offset + entry.size]

    def extract_all(self, output_dir: str, hash_db: dict[int, str] | None = None) -> int:
        """Extract all resources to output_dir, organized by type."""
        out = Path(output_dir)
        count = 0

        for entry in self.manifest.resources:
            # Resolve type name
            type_name = hex(entry.type_hash)
            if hash_db and entry.type_hash in hash_db:
                type_name = hash_db[entry.type_hash]

            # Resolve resource name
            res_name = f"{entry.resource_hash:016x}"
            if hash_db and entry.resource_hash in hash_db:
                res_name = hash_db[entry.resource_hash]

            # Determine extension
            ext = ".bin"

            type_dir = out / type_name
            type_dir.mkdir(parents=True, exist_ok=True)

            data = self.read_resource(entry.type_hash, entry.resource_hash)
            if data is not None:
                filepath = type_dir / f"{res_name}{ext}"
                with open(filepath, "wb") as f:
                    f.write(data)
                count += 1

        return count

    def clear_cache(self) -> None:
        self._frame_cache.clear()


class PackageWriter:
    """Writes modified resources back into Echo VR package files."""

    def __init__(self, manifest: Manifest):
        self.manifest = manifest
        self._modified_resources: dict[tuple[int, int], bytes] = {}

    def set_resource(self, type_hash: int, resource_hash: int, data: bytes) -> None:
        """Register a modified resource."""
        self._modified_resources[(type_hash, resource_hash)] = data

    def repack(self, original_reader: PackageReader, output_dir: str,
               compression_level: int = 3) -> Manifest:
        """Repack all resources into new package files with a new manifest.

        Reads unmodified resources from original_reader, uses modified
        resources from set_resource() calls.
        """
        if not HAS_ZSTD:
            raise ImportError("zstandard package required: pip install zstandard")

        out = Path(output_dir)
        out.mkdir(parents=True, exist_ok=True)

        cctx = zstd.ZstdCompressor(level=compression_level)
        new_manifest = Manifest()
        new_manifest.package_count = self.manifest.package_count
        new_manifest.table_descriptors = list(self.manifest.table_descriptors)

        # Group resources by frame
        frame_resources: dict[int, list[int]] = {}
        for i, entry in enumerate(self.manifest.resources):
            frame_resources.setdefault(entry.frame_index, []).append(i)

        # Repack frame by frame
        pkg_path = out / f"{original_reader.manifest_name}_0"
        cumulative_offset = 0
        new_frames = []
        new_resources = []
        new_extended = []

        with open(pkg_path, "wb") as pkg_file:
            for frame_idx in sorted(frame_resources.keys()):
                resource_indices = frame_resources[frame_idx]

                # Build frame data
                frame_data = bytearray()
                frame_entries = []

                for res_idx in resource_indices:
                    orig = self.manifest.resources[res_idx]
                    key = (orig.type_hash, orig.resource_hash)

                    if key in self._modified_resources:
                        res_data = self._modified_resources[key]
                    else:
                        res_data = original_reader.read_resource(orig.type_hash, orig.resource_hash)
                        if res_data is None:
                            continue

                    offset_in_frame = len(frame_data)
                    frame_data.extend(res_data)

                    from .manifest import ResourceIndexEntry
                    new_entry = ResourceIndexEntry(
                        type_hash=orig.type_hash,
                        resource_hash=orig.resource_hash,
                        frame_index=len(new_frames),
                        offset=offset_in_frame,
                        size=len(res_data),
                        package_index=0,
                    )
                    frame_entries.append(new_entry)

                    # Copy extended info
                    if res_idx < len(self.manifest.extended_info):
                        new_extended.append(self.manifest.extended_info[res_idx])

                # Compress frame
                compressed = cctx.compress(bytes(frame_data))

                new_frame = FrameEntry(
                    package_path_index=0,
                    cumulative_offset=cumulative_offset,
                    compressed_size=len(compressed),
                    decompressed_size=len(frame_data),
                )
                new_frames.append(new_frame)
                new_resources.extend(frame_entries)

                pkg_file.write(compressed)
                cumulative_offset += len(compressed)

        # Sort resources by (type_hash, resource_hash) for binary search
        new_resources.sort(key=lambda e: (e.type_hash, e.resource_hash))
        new_extended.sort(key=lambda e: (e.type_hash, e.resource_hash))

        new_manifest.resources = new_resources
        new_manifest.extended_info = new_extended
        new_manifest.frames = new_frames

        return new_manifest
