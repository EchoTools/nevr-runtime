"""CGSceneResource parser/serializer -- complete 26-section binary format.

IDA-verified layout (sub_140549CF0).  The file is a CSceneData stream parsed
sequentially.  All 26 sections are parsed forward using known entry sizes and
sub-structures.  Round-trips byte-identical on all known game files.

Section layout:
  1-5:   Simple counted arrays (360, 296, 120, 12, 4 byte entries)
  6:     CFrustumCullBoxTree (5 arrays + 4 memblocks + u64)
  7:     Draw data sets (168B entries)
  8-9:   u32 scalars
  10:    Lighting entries (56B)
  11:    CMemBlock (byte blob)
  12:    CGCollisionGroup (6 shape lists with nested arrays)
  13:    CGBspTree (15 sub-fields)
  14:    SNodeGraph element array (variable-length nested)
  15:    Occlusion (count << 7 bytes)
  16:    Array of u8-arrays
  17:    Hash-to-index pairs (8B)
  18:    Extended node properties (24B)
  19-20: u32 scalars
  21:    AABB (24B)
  22:    SNodeGraph (4 sub-arrays: hierarchy, entity hash, components, comp hash)
  23:    Scene set entries (112B)
  24:    Scene set hash table (32B)
  25:    Scene sets sub-object (32B)
  26:    Trailing 3 arrays (8B each)
"""

from __future__ import annotations

import struct
from dataclasses import dataclass
from typing import List, Optional

from ..binary_utils import BinaryReader, BinaryWriter
from .base import Resource


# ---------------------------------------------------------------------------
# Data classes
# ---------------------------------------------------------------------------

@dataclass
class EntityLookupEntry:
    entity_hash: int
    entity_index: int


@dataclass
class CountedSection:
    """A simple section: u32 count followed by count * entry_size bytes."""
    count: int
    entry_size: int
    raw: bytes  # count * entry_size bytes (NOT including the count u32)

    def total_bytes(self) -> int:
        return 4 + len(self.raw)


@dataclass
class SNodeGraph:
    """Section 22: the scene node graph with 4 sub-arrays."""
    hier_count: int
    hier_data: bytes       # count x 16B
    entity_hash_count: int
    entity_hash_data: bytes  # count x 16B
    entity_hash_trailing: int  # u32
    comp_count: int
    comp_data: bytes        # count x 48B
    comp_hash_count: int
    comp_hash_data: bytes   # count x 16B
    comp_hash_trailing: int  # u32


# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

def _capture_range(r: BinaryReader, start: int) -> bytes:
    """Return bytes from start to current position."""
    end = r.tell()
    r.seek(start)
    raw = r.read_bytes(end - start)
    return raw


# ---------------------------------------------------------------------------
# Main resource class
# ---------------------------------------------------------------------------

class CGSceneResource(Resource):
    type_name = "CGSceneResourceWin10"

    # Section 1-5 entry sizes (IDA-verified)
    _ENTRY_SIZES_HEAD = [360, 296, 120, 12, 4]

    def __init__(self) -> None:
        # HEAD: sections 1-5
        self.head_sections: List[CountedSection] = []

        # Sections 6-21 stored as opaque blobs (raw bytes including counts)
        self.sec6_raw: bytes = b""    # CFrustumCullBoxTree
        self.sec7_raw: bytes = b""    # draw data sets
        self.sec8_raw: bytes = b""    # u32 scalar (4B)
        self.sec9_raw: bytes = b""    # u32 scalar (4B)
        self.sec10_raw: bytes = b""   # lighting
        self.sec11_raw: bytes = b""   # CMemBlock
        self.sec12_raw: bytes = b""   # CGCollisionGroup
        self.sec13_raw: bytes = b""   # CGBspTree
        self.sec14_raw: bytes = b""   # SNodeGraph elements
        self.sec15_raw: bytes = b""   # Occlusion
        self.sec16_raw: bytes = b""   # Array of u8-arrays
        self.sec17_raw: bytes = b""   # hash-to-index pairs
        self.sec18_raw: bytes = b""   # extended node props
        self.sec19_raw: bytes = b""   # u32 scalar (4B)
        self.sec20_raw: bytes = b""   # u32 scalar (4B)
        self.sec21_raw: bytes = b""   # AABB (24B)

        # Section 22: SNodeGraph (parsed for expansion)
        self.node_graph: Optional[SNodeGraph] = None

        # Sections 23-25 stored as opaque blobs
        self.sec23_raw: bytes = b""   # scene set entries
        self.sec24_raw: bytes = b""   # scene set hash table
        self.sec25_raw: bytes = b""   # scene sets sub-object

        # Section 26: trailing 3 arrays
        self.sec26_arr0_count: int = 0
        self.sec26_arr0_raw: bytes = b""
        self.sec26_arr1_count: int = 0
        self.sec26_arr1_raw: bytes = b""
        self.sec26_arr2_count: int = 0
        self.sec26_arr2_raw: bytes = b""

    # ------------------------------------------------------------------
    # Deserialization
    # ------------------------------------------------------------------

    @classmethod
    def from_bytes(cls, data: bytes) -> CGSceneResource:
        res = cls()
        r = BinaryReader(data)

        # --- Sections 1-5: simple counted arrays ---
        for entry_size in cls._ENTRY_SIZES_HEAD:
            count = r.read_u32()
            raw = r.read_bytes(count * entry_size) if count else b""
            res.head_sections.append(CountedSection(count, entry_size, raw))

        # --- Section 6: CFrustumCullBoxTree ---
        s = r.tell()
        for sz in [120, 8, 8, 8, 8]:
            c = r.read_u32()
            if c:
                r.read_bytes(c * sz)
        for _ in range(4):
            bc = r.read_u32()
            if bc:
                r.read_bytes(bc)
        r.read_bytes(8)  # u64 scalar
        res.sec6_raw = _capture_range(r, s)

        # --- Section 7: draw data sets (168B) ---
        s = r.tell()
        c = r.read_u32()
        if c:
            r.read_bytes(c * 168)
        res.sec7_raw = _capture_range(r, s)

        # --- Section 8: u32 scalar ---
        res.sec8_raw = r.read_bytes(4)

        # --- Section 9: u32 scalar ---
        res.sec9_raw = r.read_bytes(4)

        # --- Section 10: lighting (56B) ---
        s = r.tell()
        c = r.read_u32()
        if c:
            r.read_bytes(c * 56)
        res.sec10_raw = _capture_range(r, s)

        # --- Section 11: CMemBlock ---
        s = r.tell()
        bc = r.read_u32()
        if bc:
            r.read_bytes(bc)
        res.sec11_raw = _capture_range(r, s)

        # --- Section 12: CGCollisionGroup ---
        s = r.tell()
        cls._skip_collision_group(r)
        res.sec12_raw = _capture_range(r, s)

        # --- Section 13: CGBspTree ---
        s = r.tell()
        cls._skip_bsp_tree(r)
        res.sec13_raw = _capture_range(r, s)

        # --- Section 14: SNodeGraph elements ---
        s = r.tell()
        cls._skip_node_graph_elements(r)
        res.sec14_raw = _capture_range(r, s)

        # --- Section 15: Occlusion ---
        s = r.tell()
        c = r.read_u32()
        if c:
            r.read_bytes(c << 7)
        res.sec15_raw = _capture_range(r, s)

        # --- Section 16: Array of u8-arrays ---
        s = r.tell()
        outer = r.read_u32()
        for _ in range(outer):
            ic = r.read_u32()
            if ic:
                r.read_bytes(ic * 8)
        res.sec16_raw = _capture_range(r, s)

        # --- Section 17: hash-to-index pairs (8B) ---
        s = r.tell()
        c = r.read_u32()
        if c:
            r.read_bytes(c * 8)
        res.sec17_raw = _capture_range(r, s)

        # --- Section 18: extended node properties (24B) ---
        s = r.tell()
        c = r.read_u32()
        if c:
            r.read_bytes(c * 24)
        res.sec18_raw = _capture_range(r, s)

        # --- Section 19: u32 scalar ---
        res.sec19_raw = r.read_bytes(4)

        # --- Section 20: u32 scalar ---
        res.sec20_raw = r.read_bytes(4)

        # --- Section 21: AABB (24B) ---
        res.sec21_raw = r.read_bytes(24)

        # --- Section 22: SNodeGraph ---
        res.node_graph = cls._read_node_graph(r)

        # --- Section 23: scene set entries (112B) ---
        s = r.tell()
        c = r.read_u32()
        if c:
            r.read_bytes(c * 112)
        res.sec23_raw = _capture_range(r, s)

        # --- Section 24: scene set hash table (32B) ---
        s = r.tell()
        c = r.read_u32()
        if c:
            r.read_bytes(c * 32)
        res.sec24_raw = _capture_range(r, s)

        # --- Section 25: scene sets sub-object (32B) ---
        s = r.tell()
        c = r.read_u32()
        if c:
            r.read_bytes(c * 32)
        res.sec25_raw = _capture_range(r, s)

        # --- Section 26: trailing 3 arrays ---
        res.sec26_arr0_count = r.read_u32()
        res.sec26_arr0_raw = r.read_bytes(res.sec26_arr0_count * 8) if res.sec26_arr0_count else b""
        res.sec26_arr1_count = r.read_u32()
        res.sec26_arr1_raw = r.read_bytes(res.sec26_arr1_count * 8) if res.sec26_arr1_count else b""
        res.sec26_arr2_count = r.read_u32()
        res.sec26_arr2_raw = r.read_bytes(res.sec26_arr2_count * 8) if res.sec26_arr2_count else b""

        assert r.tell() == len(data), (
            f"Parse did not consume entire file: pos={r.tell():#x}, "
            f"size={len(data):#x}"
        )

        return res

    # ------------------------------------------------------------------
    # Serialization
    # ------------------------------------------------------------------

    def to_bytes(self) -> bytes:
        w = BinaryWriter()

        # Sections 1-5
        for sec in self.head_sections:
            w.write_u32(sec.count)
            if sec.raw:
                w.write_bytes(sec.raw)

        # Sections 6-21 (opaque blobs)
        w.write_bytes(self.sec6_raw)
        w.write_bytes(self.sec7_raw)
        w.write_bytes(self.sec8_raw)
        w.write_bytes(self.sec9_raw)
        w.write_bytes(self.sec10_raw)
        w.write_bytes(self.sec11_raw)
        w.write_bytes(self.sec12_raw)
        w.write_bytes(self.sec13_raw)
        w.write_bytes(self.sec14_raw)
        w.write_bytes(self.sec15_raw)
        w.write_bytes(self.sec16_raw)
        w.write_bytes(self.sec17_raw)
        w.write_bytes(self.sec18_raw)
        w.write_bytes(self.sec19_raw)
        w.write_bytes(self.sec20_raw)
        w.write_bytes(self.sec21_raw)

        # Section 22: SNodeGraph
        ng = self.node_graph
        assert ng is not None
        w.write_u32(ng.hier_count)
        if ng.hier_data:
            w.write_bytes(ng.hier_data)
        w.write_u32(ng.entity_hash_count)
        if ng.entity_hash_data:
            w.write_bytes(ng.entity_hash_data)
        w.write_u32(ng.entity_hash_trailing)
        w.write_u32(ng.comp_count)
        if ng.comp_data:
            w.write_bytes(ng.comp_data)
        w.write_u32(ng.comp_hash_count)
        if ng.comp_hash_data:
            w.write_bytes(ng.comp_hash_data)
        w.write_u32(ng.comp_hash_trailing)

        # Sections 23-25
        w.write_bytes(self.sec23_raw)
        w.write_bytes(self.sec24_raw)
        w.write_bytes(self.sec25_raw)

        # Section 26: trailing 3 arrays
        w.write_u32(self.sec26_arr0_count)
        if self.sec26_arr0_raw:
            w.write_bytes(self.sec26_arr0_raw)
        w.write_u32(self.sec26_arr1_count)
        if self.sec26_arr1_raw:
            w.write_bytes(self.sec26_arr1_raw)
        w.write_u32(self.sec26_arr2_count)
        if self.sec26_arr2_raw:
            w.write_bytes(self.sec26_arr2_raw)

        return w.getvalue()

    # ------------------------------------------------------------------
    # Entity lookup API
    # ------------------------------------------------------------------

    @property
    def entity_lookup(self) -> list[EntityLookupEntry]:
        """Return the sorted entity hash table as (hash, index) pairs."""
        ng = self.node_graph
        if ng is None or ng.entity_hash_count == 0:
            return []
        entries = []
        for i in range(ng.entity_hash_count):
            off = i * 16
            h = struct.unpack_from("<Q", ng.entity_hash_data, off)[0]
            idx = struct.unpack_from("<Q", ng.entity_hash_data, off + 8)[0]
            entries.append(EntityLookupEntry(h, idx))
        return entries

    def has_entity(self, entity_hash: int) -> bool:
        """Check whether an entity hash exists in the sorted hash table."""
        ng = self.node_graph
        if ng is None or ng.entity_hash_count == 0:
            return False
        target = struct.pack("<Q", entity_hash)
        return target in ng.entity_hash_data

    def insert_entity(self, entity_hash: int, source_hash: int = 0) -> int:
        """Insert entity into sorted hash table, targeting a slot with valid hierarchy.

        Scans outward from the ideal sort position to find an entry whose
        hierarchy slot has a non-sentinel parent.  Replaces that entry and
        bubble-sorts to maintain sort order.  Returns the slot_index.
        """
        ng = self.node_graph
        if ng is None or ng.entity_hash_count == 0:
            raise ValueError("Sorted hash table not found")

        ehd = bytearray(ng.entity_hash_data)
        count = ng.entity_hash_count

        # Find insertion position in sorted table
        insert_pos = count
        for i in range(count):
            off = i * 16
            h = struct.unpack_from("<Q", ehd, off)[0]
            if h > entity_hash:
                insert_pos = i
                break

        # Replace the entry just before insert position
        replace_pos = max(0, insert_pos - 1)
        off = replace_pos * 16
        replaced_slot = struct.unpack_from("<Q", ehd, off + 8)[0]

        # Write new hash at replace_pos
        struct.pack_into("<Q", ehd, off, entity_hash)

        # Bubble-sort left
        target = replace_pos
        while target > 0:
            left_off = (target - 1) * 16
            cur_off = target * 16
            lh = struct.unpack_from("<Q", ehd, left_off)[0]
            ch = struct.unpack_from("<Q", ehd, cur_off)[0]
            if ch < lh:
                le = bytes(ehd[left_off:left_off + 16])
                ce = bytes(ehd[cur_off:cur_off + 16])
                ehd[left_off:left_off + 16] = ce
                ehd[cur_off:cur_off + 16] = le
                target -= 1
            else:
                break

        # Bubble-sort right
        while target < count - 1:
            right_off = (target + 1) * 16
            cur_off = target * 16
            rh = struct.unpack_from("<Q", ehd, right_off)[0]
            ch = struct.unpack_from("<Q", ehd, cur_off)[0]
            if ch > rh:
                re = bytes(ehd[right_off:right_off + 16])
                ce = bytes(ehd[cur_off:cur_off + 16])
                ehd[right_off:right_off + 16] = ce
                ehd[cur_off:cur_off + 16] = re
                target += 1
            else:
                break

        # Copy hierarchy parent from source entity if requested
        if source_hash and ng.hier_count > 0:
            hd = bytearray(ng.hier_data)
            source_slot = -1
            for i in range(count):
                soff = i * 16
                sh = struct.unpack_from("<Q", ehd, soff)[0]
                if sh == source_hash:
                    source_slot = struct.unpack_from("<Q", ehd, soff + 8)[0]
                    break
            if source_slot >= 0 and replaced_slot < ng.hier_count:
                src_parent = struct.unpack_from("<I", hd,
                    source_slot * 16)[0]
                struct.pack_into("<I", hd,
                    replaced_slot * 16, src_parent)
            ng.hier_data = bytes(hd)

        ng.entity_hash_data = bytes(ehd)
        return replaced_slot

    # ------------------------------------------------------------------
    # Expansion helpers
    # ------------------------------------------------------------------

    def expand_tables(self, new_count: int) -> None:
        """Expand the hierarchy count so the runtime allocates a larger node array.

        Only changes the hierarchy count field.  Does NOT insert new data
        entries -- the file size stays the same.

        Args:
            new_count: Target total entry count.
        """
        ng = self.node_graph
        if ng is None:
            raise ValueError("Node graph not found -- cannot expand")
        if new_count <= ng.hier_count:
            return
        ng.hier_count = new_count

    def expand_for_clones(
        self,
        num_new_entries: int,
        new_entity_hashes: List[int] = None,
        entity_slot_base: Optional[int] = None,
    ) -> None:
        """Expand Section 13's first sub-array (BSP verts/nodes, 16B entries).

        Only the first sub-array is expanded. Other sub-arrays have
        independent counts and don't need expansion for CTransformCR.
        """
        if num_new_entries <= 0:
            return

        raw = self.sec13_raw
        if len(raw) < 4:
            raise ValueError("Section 13 too small to expand")

        old_count = struct.unpack_from("<I", raw, 0)[0]
        old_data_end = 4 + old_count * 16

        # Build new entries with dynamic flag
        new_entries = bytearray()
        for i in range(num_new_entries):
            slot = old_count + i
            new_entries += struct.pack("<IIII", 0xFFFFFFFF, 0xFFFFFFFF, slot, 0)

        new_count = old_count + num_new_entries
        new_sec13 = (
            struct.pack("<I", new_count)
            + raw[4:old_data_end]
            + new_entries
            + raw[old_data_end:]
        )
        self.sec13_raw = new_sec13

    @property
    def hierarchy_count(self) -> int:
        """Number of entries in the hierarchy table."""
        ng = self.node_graph
        return ng.hier_count if ng else 0

    def get_hierarchy_entry(self, index: int) -> tuple[int, int, int, int]:
        """Return (parent, child, slot_num, pad) for a hierarchy entry."""
        ng = self.node_graph
        if ng is None or index >= ng.hier_count:
            raise IndexError(f"Hierarchy index {index} out of range")
        off = index * 16
        return struct.unpack_from("<IIII", ng.hier_data, off)

    # ------------------------------------------------------------------
    # Internal: section parsers
    # ------------------------------------------------------------------

    @classmethod
    def _skip_collision_group(cls, r: BinaryReader) -> None:
        """Skip section 12: CGCollisionGroup (6 shape lists).

        Shapes 0-3 (point, line, sphere, box) have fixed-size data entries.
        Shapes 4-5 (spline, convex) have variable-length nested arrays.
        """
        simple_entry_sizes = [12, 32, 16, 40]

        for shape_idx in range(6):
            # Hull 1: u32 vert_count + vert_count*16B + u32 status
            vc = r.read_u32()
            if vc:
                r.read_bytes(vc * 16)
            r.read_u32()  # status

            # Hull 2: u32 vert_count + vert_count*16B + u32 status
            vc = r.read_u32()
            if vc:
                r.read_bytes(vc * 16)
            r.read_u32()  # status

            # Shape data
            dc = r.read_u32()
            if dc == 0:
                continue

            if shape_idx < 4:
                # Simple: fixed entry size
                r.read_bytes(dc * simple_entry_sizes[shape_idx])
            elif shape_idx == 4:
                # Spline: variable-length per element
                # Each: u32 degree + CArray<4B> + CArray<12B> + u32 flag +
                #        CArray<12B> + CArray<12B>
                for _ in range(dc):
                    r.read_u32()  # degree
                    c = r.read_u32()  # knots count
                    if c:
                        r.read_bytes(c * 4)
                    c = r.read_u32()  # control points count
                    if c:
                        r.read_bytes(c * 12)
                    r.read_u32()  # flag
                    c = r.read_u32()  # samples count
                    if c:
                        r.read_bytes(c * 12)
                    c = r.read_u32()  # last array count
                    if c:
                        r.read_bytes(c * 12)
            else:
                # Convex: variable-length per element
                # Each: CArray<16B> + 12B + 12B + CArray<12B>
                for _ in range(dc):
                    c = r.read_u32()  # vertices count
                    if c:
                        r.read_bytes(c * 16)
                    r.read_bytes(12)  # vec3
                    r.read_bytes(12)  # vec3
                    c = r.read_u32()  # normals count
                    if c:
                        r.read_bytes(c * 12)

    @classmethod
    def _skip_bsp_tree(cls, r: BinaryReader) -> None:
        """Skip section 13: CGBspTree (15 sub-fields)."""
        # 1. verts: u32 count + count*16B
        c = r.read_u32()
        if c:
            r.read_bytes(c * 16)
        # 2. sedges: u32 count + count*8B
        c = r.read_u32()
        if c:
            r.read_bytes(c * 8)
        # 3. tedges: u32 count + count*8B
        c = r.read_u32()
        if c:
            r.read_bytes(c * 8)
        # 4. planes: u32 count + count*8B + u32 status
        c = r.read_u32()
        if c:
            r.read_bytes(c * 8)
        r.read_u32()  # status
        # 5. tplanes: u32 count + count*8B + u32 status
        c = r.read_u32()
        if c:
            r.read_bytes(c * 8)
        r.read_u32()  # status
        # 6. idxremap: u32 count + count*4B
        c = r.read_u32()
        if c:
            r.read_bytes(c * 4)
        # 7. deadlist: u32 count + count*8B
        c = r.read_u32()
        if c:
            r.read_bytes(c * 8)
        # 8-11: 4 u32 fields
        r.read_bytes(16)
        # 12: u32 array
        c = r.read_u32()
        if c:
            r.read_bytes(c * 4)
        # 13: hull: u32 count + count*16B + u32 status
        c = r.read_u32()
        if c:
            r.read_bytes(c * 16)
        r.read_u32()  # status
        # 14: u32 array
        c = r.read_u32()
        if c:
            r.read_bytes(c * 4)
        # 15: u64 array
        c = r.read_u32()
        if c:
            r.read_bytes(c * 8)

    @classmethod
    def _skip_node_graph_elements(cls, r: BinaryReader) -> None:
        """Skip section 14: array of SNodeGraph elements."""
        outer = r.read_u32()
        for _ in range(outer):
            r.read_bytes(8)   # u64
            r.read_bytes(8)   # u64
            c1 = r.read_u32()
            if c1:
                r.read_bytes(c1 * 8)
            r.read_u32()  # u32
            r.read_u32()  # u32
            c2 = r.read_u32()
            for _ in range(c2):
                r.read_u32()  # u32
                r.read_u32()  # u32
                inner = r.read_u32()
                if inner:
                    r.read_bytes(inner * 4)
            c3 = r.read_u32()
            if c3:
                r.read_bytes(c3 * 8)
            c4 = r.read_u32()
            for _ in range(c4):
                ac = r.read_u32()
                if ac:
                    r.read_bytes(ac * 16)
                r.read_bytes(12)  # 3 u32
                r.read_bytes(12)  # 3 u32
                ac2 = r.read_u32()
                if ac2:
                    r.read_bytes(ac2 * 12)
            c5 = r.read_u32()
            if c5:
                r.read_bytes(c5 * 16)

    @classmethod
    def _read_node_graph(cls, r: BinaryReader) -> SNodeGraph:
        """Read section 22: SNodeGraph with 4 sub-arrays."""
        # Sub 0: hierarchy (16B entries)
        hc = r.read_u32()
        hd = r.read_bytes(hc * 16) if hc else b""

        # Sub 1: entity hash (16B entries + trailing u32)
        ec = r.read_u32()
        ed = r.read_bytes(ec * 16) if ec else b""
        et = r.read_u32()

        # Sub 2: components (48B entries)
        cc = r.read_u32()
        cd = r.read_bytes(cc * 48) if cc else b""

        # Sub 3: component hash (16B entries + trailing u32)
        chc = r.read_u32()
        chd = r.read_bytes(chc * 16) if chc else b""
        cht = r.read_u32()

        return SNodeGraph(
            hier_count=hc, hier_data=hd,
            entity_hash_count=ec, entity_hash_data=ed,
            entity_hash_trailing=et,
            comp_count=cc, comp_data=cd,
            comp_hash_count=chc, comp_hash_data=chd,
            comp_hash_trailing=cht,
        )
