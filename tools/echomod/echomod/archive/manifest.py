"""Echo VR manifest parser and writer.

Manifest file format:
  [4B] Magic "ZSTD"
  [4B] Header size (24)
  [8B] Decompressed size
  [8B] Compressed size
  [... ] ZSTD-compressed payload

Decompressed payload contains 3 tables of RadArrayDescriptor56-like headers:
  Table 0: ResourceIndexEntry[69712] (32B each) — resource -> frame mapping
  Table 1: ExtendedInfoEntry[69712] (40B each) — extended resource info
  Table 2: FrameEntry[10326] (16B each) — frame -> package offset mapping
"""

from __future__ import annotations
import struct
from dataclasses import dataclass, field
from ..binary_utils import BinaryReader, BinaryWriter

try:
    import zstandard as zstd
    HAS_ZSTD = True
except ImportError:
    HAS_ZSTD = False


MANIFEST_MAGIC = b"ZSTD"
MANIFEST_HEADER_SIZE = 24


@dataclass
class ResourceIndexEntry:
    """32-byte resource location entry."""
    type_hash: int        # +0x00 u64
    resource_hash: int    # +0x08 u64
    frame_index: int      # +0x10 u32
    offset: int           # +0x14 u32 — offset within decompressed frame
    size: int             # +0x18 u32 — resource size in bytes
    package_index: int    # +0x1C u32

    SIZE = 32


@dataclass
class ExtendedInfoEntry:
    """40-byte extended info entry."""
    type_hash: int
    resource_hash: int
    extra_data: bytes  # 24 bytes of additional data

    SIZE = 40


@dataclass
class FrameEntry:
    """16-byte frame table entry."""
    package_path_index: int     # +0x00 u32
    cumulative_offset: int      # +0x04 u32 — byte offset into concatenated packages
    compressed_size: int        # +0x08 u32
    decompressed_size: int      # +0x0C u32

    SIZE = 16


@dataclass
class TableDescriptor:
    """Manifest table descriptor. Layout observed from manifest dump:
    +0:  u64 pData (0 on disk)
    +8:  u64 total_byte_size (count * stride)
    +16: u64 pAllocator (0 on disk)
    +24: u32 pad0 + u32 flags
    +32: u64 entry_stride
    +40: u64 entry_count
    +48: u64 capacity (= entry_count)
    Total: 56 bytes

    BUT there are 2 extra u64 padding fields between descriptors, making the
    effective spacing 64 bytes per descriptor + 8 bytes at the end.
    We handle this by reading 8 fields (64 bytes) and treating field 0 as part
    of the previous descriptor's trailing data OR a separate pad.

    Actual observed layout in manifest (3 descriptors starting at offset 8):
    Desc 0: offsets 8-63  (56 bytes)
    Desc 1: offsets 64-127 (includes 8 bytes gap)
    Desc 2: offsets 128-191 (includes 8 bytes gap + trailing)
    """
    raw: bytes = b""
    total_byte_size: int = 0
    entry_stride: int = 0
    entry_count: int = 0
    capacity: int = 0

    SIZE = 56

    @classmethod
    def from_reader(cls, r: BinaryReader) -> TableDescriptor:
        raw = r.read_bytes(56)
        import struct
        # Parse known fields from the 56-byte block
        # +0: pad (0)
        # +8: total_byte_size
        # +16: pad (0)
        # +24: pad+flags (0, 0)
        # +32: stride
        # +40: count
        # +48: capacity
        total = struct.unpack_from("<Q", raw, 8)[0]
        stride = struct.unpack_from("<Q", raw, 32)[0]
        count = struct.unpack_from("<Q", raw, 40)[0]
        cap = struct.unpack_from("<Q", raw, 48)[0]
        return cls(raw=raw, total_byte_size=total, entry_stride=stride,
                   entry_count=count, capacity=cap)

    def to_writer(self, w: BinaryWriter) -> None:
        if self.raw:
            # Preserve original bytes with updated fields
            import struct
            buf = bytearray(self.raw)
            struct.pack_into("<Q", buf, 8, self.total_byte_size)
            struct.pack_into("<Q", buf, 32, self.entry_stride)
            struct.pack_into("<Q", buf, 40, self.entry_count)
            struct.pack_into("<Q", buf, 48, self.capacity)
            w.write_bytes(bytes(buf))
        else:
            import struct
            buf = bytearray(56)
            struct.pack_into("<Q", buf, 8, self.total_byte_size)
            struct.pack_into("<Q", buf, 32, self.entry_stride)
            struct.pack_into("<Q", buf, 40, self.entry_count)
            struct.pack_into("<Q", buf, 48, self.capacity)
            w.write_bytes(bytes(buf))


class Manifest:
    """Parsed Echo VR manifest."""

    def __init__(self):
        self.package_count: int = 0
        self.table_descriptors: list[TableDescriptor] = []
        self.resources: list[ResourceIndexEntry] = []
        self.extended_info: list[ExtendedInfoEntry] = []
        self.frames: list[FrameEntry] = []
        self._raw_decompressed: bytes = b""

    @classmethod
    def from_file(cls, path: str) -> Manifest:
        if not HAS_ZSTD:
            raise ImportError("zstandard package required: pip install zstandard")

        with open(path, "rb") as f:
            raw = f.read()

        return cls.from_bytes(raw)

    @classmethod
    def from_bytes(cls, data: bytes) -> Manifest:
        if not HAS_ZSTD:
            raise ImportError("zstandard package required: pip install zstandard")

        # Parse ZSTD wrapper header
        magic = data[:4]
        if magic != MANIFEST_MAGIC:
            raise ValueError(f"Not a ZSTD manifest: magic={magic!r}")

        header_size = struct.unpack_from("<I", data, 4)[0]
        decompressed_size = struct.unpack_from("<Q", data, 8)[0]
        compressed_size = struct.unpack_from("<Q", data, 16)[0]

        # Decompress
        compressed = data[header_size:]
        dctx = zstd.ZstdDecompressor()
        decompressed = dctx.decompress(compressed, max_output_size=decompressed_size + 1024)

        return cls._parse_decompressed(decompressed)

    @classmethod
    def _parse_decompressed(cls, data: bytes) -> Manifest:
        m = Manifest()
        m._raw_decompressed = data
        r = BinaryReader(data)

        # Package count
        m.package_count = r.read_u64()

        # Read 3 table descriptors (56 bytes each + 8 bytes gap between)
        for i in range(3):
            m.table_descriptors.append(TableDescriptor.from_reader(r))
            if i < 2:
                r.read_bytes(8)  # 8-byte gap between descriptors

        # Tables follow sequentially: resource index, extended info, frame table
        desc0 = m.table_descriptors[0]
        num_resources = desc0.entry_count

        # Table 0: Resource index (32B entries)
        for _ in range(num_resources):
            entry = ResourceIndexEntry(
                type_hash=r.read_u64(),
                resource_hash=r.read_u64(),
                frame_index=r.read_u32(),
                offset=r.read_u32(),
                size=r.read_u32(),
                package_index=r.read_u32(),
            )
            m.resources.append(entry)

        # Table 1: Extended info (40B entries)
        desc1 = m.table_descriptors[1]
        for _ in range(desc1.entry_count):
            th = r.read_u64()
            rh = r.read_u64()
            extra = r.read_bytes(24)
            m.extended_info.append(ExtendedInfoEntry(th, rh, extra))

        # Table 2: Frame table (16B entries)
        desc2 = m.table_descriptors[2]
        for _ in range(desc2.entry_count):
            entry = FrameEntry(
                package_path_index=r.read_u32(),
                cumulative_offset=r.read_u32(),
                compressed_size=r.read_u32(),
                decompressed_size=r.read_u32(),
            )
            m.frames.append(entry)

        # Remaining bytes (typically 16 bytes of trailing metadata)
        m._trailing = r.read_bytes(r.remaining()) if r.remaining() > 0 else b""

        return m

    def find_resource(self, type_hash: int, resource_hash: int) -> ResourceIndexEntry | None:
        """Binary search for a resource by type_hash + resource_hash."""
        lo, hi = 0, len(self.resources) - 1
        while lo <= hi:
            mid = (lo + hi) // 2
            e = self.resources[mid]
            key = (e.type_hash, e.resource_hash)
            target = (type_hash, resource_hash)
            if key == target:
                return e
            elif key < target:
                lo = mid + 1
            else:
                hi = mid - 1
        return None

    def get_resource_location(self, type_hash: int, resource_hash: int) -> tuple[int, int, int] | None:
        """Get (package_file_offset, compressed_frame_size, offset_within_frame) for a resource."""
        entry = self.find_resource(type_hash, resource_hash)
        if entry is None:
            return None
        frame = self.frames[entry.frame_index]
        return frame.cumulative_offset, frame.compressed_size, entry.offset
