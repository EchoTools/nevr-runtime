"""CActorDataResource parser/serializer.

Binary layout (stream order):
  [784 bytes] CActorDataResourceInner (13 array descriptors)
  [variable]  headers data: ActorDataHeaderEntry * headers.count (16B each)
  [variable]  components data: ActorDataComponentEntry * components.count (64B each)
              followed by inline uint16[] actorIndices for each component
  [variable]  nodeids: uint64 * nodeids.count
  [variable]  names: uint64 * names.count
  [variable]  prefabids: uint64 * prefabids.count
  [variable]  transforms: ActorDataTransform * transforms.count (48B each)
  [variable]  numpooled: uint16 * numpooled.count
  [variable]  parents: uint16 * parents.count
  [variable]  bits0..bits4: uint64 * bitsN.count (x5 arrays)
"""

from __future__ import annotations
import math
from dataclasses import dataclass, field
from ..binary_utils import BinaryReader, BinaryWriter
from ..descriptors import RadArrayDescriptor56, RadArrayDescriptor64
from .base import Resource

INNER_SIZE = 784


@dataclass
class ActorDataHeaderEntry:
    """16-byte component type header."""
    type_hash: int   # +0x00
    count: int       # +0x08


@dataclass
class ActorDataComponentEntry:
    """Component entry: 64-byte descriptor header + inline uint16 actor indices."""
    type_hash: int                       # +0x00
    indices_descriptor: RadArrayDescriptor56  # +0x08 (56 bytes)
    actor_indices: list[int]             # inline uint16[] read from stream


@dataclass
class ActorDataTransform:
    """48-byte local transform."""
    rotation_x: float
    rotation_y: float
    rotation_z: float
    rotation_w: float
    position_x: float
    position_y: float
    position_z: float
    scale_x: float
    scale_y: float
    scale_z: float
    pad0: int
    pad1: int


class ActorDataResource(Resource):
    """CActorDataResource: 784-byte inner header + 13 sequential data arrays."""

    type_name = "CActorDataResourceWin10"

    def __init__(self):
        # Descriptors (13 total)
        self.desc_headers = RadArrayDescriptor64()
        self.desc_components = RadArrayDescriptor64()
        self.desc_nodeids = RadArrayDescriptor56()
        self.desc_names = RadArrayDescriptor56()
        self.desc_prefabids = RadArrayDescriptor56()
        self.desc_transforms = RadArrayDescriptor56()
        self.desc_numpooled = RadArrayDescriptor56()
        self.desc_parents = RadArrayDescriptor56()
        self.desc_bits = [RadArrayDescriptor64() for _ in range(5)]
        # Data arrays
        self.headers: list[ActorDataHeaderEntry] = []
        self.components: list[ActorDataComponentEntry] = []
        self.nodeids: list[int] = []
        self.names: list[int] = []
        self.prefabids: list[int] = []
        self.transforms: list[ActorDataTransform] = []
        self.numpooled: list[int] = []
        self.parents: list[int] = []
        self.bits: list[list[int]] = [[] for _ in range(5)]
        # Track alignment bytes between sections for round-trip fidelity
        self._alignment_bytes: list[bytes] = []

    @property
    def actor_count(self) -> int:
        return len(self.nodeids)

    @classmethod
    def from_bytes(cls, data: bytes) -> ActorDataResource:
        r = BinaryReader(data)
        res = cls()

        # Read 784-byte inner header (13 descriptors)
        res.desc_headers = RadArrayDescriptor64.from_reader(r)
        res.desc_components = RadArrayDescriptor64.from_reader(r)
        res.desc_nodeids = RadArrayDescriptor56.from_reader(r)
        res.desc_names = RadArrayDescriptor56.from_reader(r)
        res.desc_prefabids = RadArrayDescriptor56.from_reader(r)
        res.desc_transforms = RadArrayDescriptor56.from_reader(r)
        res.desc_numpooled = RadArrayDescriptor56.from_reader(r)
        res.desc_parents = RadArrayDescriptor56.from_reader(r)
        for i in range(5):
            res.desc_bits[i] = RadArrayDescriptor64.from_reader(r)

        assert r.tell() == INNER_SIZE, f"Inner header read {r.tell()} bytes, expected {INNER_SIZE}"

        # Read headers (16B each)
        for _ in range(res.desc_headers.count):
            th = r.read_u64()
            cnt = r.read_u64()
            res.headers.append(ActorDataHeaderEntry(th, cnt))

        # Read components (64B descriptor each + inline uint16 arrays)
        comp_descriptors = []
        for _ in range(res.desc_components.count):
            th = r.read_u64()
            desc = RadArrayDescriptor56.from_reader(r)
            comp_descriptors.append((th, desc))

        # Read inline actor indices for each component
        for th, desc in comp_descriptors:
            indices = [r.read_u16() for _ in range(desc.count)]
            res.components.append(ActorDataComponentEntry(th, desc, indices))

        # Alignment after component indices
        if res.desc_nodeids.count > 0:
            pos = r.tell()
            aligned = (pos + 7) & ~7  # 8-byte align for u64 array
            if aligned > pos:
                r.read_bytes(aligned - pos)

        # Read nodeids (u64 each)
        res.nodeids = [r.read_u64() for _ in range(res.desc_nodeids.count)]

        # Read names (u64 each)
        res.names = [r.read_u64() for _ in range(res.desc_names.count)]

        # Read prefabids (u64 each)
        res.prefabids = [r.read_u64() for _ in range(res.desc_prefabids.count)]

        # Alignment before transforms (4-byte aligned)
        if res.desc_transforms.count > 0:
            pos = r.tell()
            aligned = (pos + 3) & ~3
            if aligned > pos:
                r.read_bytes(aligned - pos)

        # Read transforms (48B each)
        for _ in range(res.desc_transforms.count):
            rx = r.read_f32()
            ry = r.read_f32()
            rz = r.read_f32()
            rw = r.read_f32()
            px = r.read_f32()
            py = r.read_f32()
            pz = r.read_f32()
            sx = r.read_f32()
            sy = r.read_f32()
            sz = r.read_f32()
            p0 = r.read_u32()
            p1 = r.read_u32()
            res.transforms.append(ActorDataTransform(rx, ry, rz, rw, px, py, pz, sx, sy, sz, p0, p1))

        # Read numpooled (u16 each)
        res.numpooled = [r.read_u16() for _ in range(res.desc_numpooled.count)]

        # Alignment before parents
        if res.desc_parents.count > 0:
            pos = r.tell()
            aligned = (pos + 1) & ~1  # 2-byte align
            if aligned > pos:
                r.read_bytes(aligned - pos)

        # Read parents (u16 each)
        res.parents = [r.read_u16() for _ in range(res.desc_parents.count)]

        # Alignment before bitfield arrays (8-byte aligned)
        if any(res.desc_bits[i].count > 0 for i in range(5)):
            pos = r.tell()
            aligned = (pos + 7) & ~7
            if aligned > pos:
                r.read_bytes(aligned - pos)

        # Read 5 bitfield arrays (u64 each)
        for i in range(5):
            res.bits[i] = [r.read_u64() for _ in range(res.desc_bits[i].count)]

        return res

    def to_bytes(self) -> bytes:
        w = BinaryWriter()
        actor_count = self.actor_count
        bitfield_count = math.ceil(actor_count / 64) if actor_count > 0 else 0

        # Update descriptors
        self.desc_headers.update_for_count(len(self.headers), 16)
        self.desc_headers.extra_field = 0

        # Components: descriptor data_byte_size = count * 64
        self.desc_components.update_for_count(len(self.components), 64)
        self.desc_components.extra_field = 0

        self.desc_nodeids.update_for_count(actor_count, 8)
        self.desc_names.update_for_count(actor_count, 8)
        self.desc_prefabids.update_for_count(actor_count, 8)
        self.desc_transforms.update_for_count(actor_count, 48)
        self.desc_numpooled.update_for_count(actor_count, 2)
        self.desc_parents.update_for_count(actor_count, 2)
        for i in range(5):
            self.desc_bits[i].update_for_count(bitfield_count, 8)
            self.desc_bits[i].extra_field = actor_count

        # Write 784-byte inner header
        self.desc_headers.to_writer(w)
        self.desc_components.to_writer(w)
        self.desc_nodeids.to_writer(w)
        self.desc_names.to_writer(w)
        self.desc_prefabids.to_writer(w)
        self.desc_transforms.to_writer(w)
        self.desc_numpooled.to_writer(w)
        self.desc_parents.to_writer(w)
        for i in range(5):
            self.desc_bits[i].to_writer(w)

        # Write headers
        for h in self.headers:
            w.write_u64(h.type_hash)
            w.write_u64(h.count)

        # Write component descriptors (64B each)
        for comp in self.components:
            w.write_u64(comp.type_hash)
            comp.indices_descriptor.update_for_count(len(comp.actor_indices), 2)
            comp.indices_descriptor.to_writer(w)

        # Write inline actor indices for each component
        for comp in self.components:
            for idx in comp.actor_indices:
                w.write_u16(idx)

        # Align to 8 for nodeids
        w.align(8)

        # Write nodeids
        for nid in self.nodeids:
            w.write_u64(nid)

        # Write names
        for name in self.names:
            w.write_u64(name)

        # Write prefabids
        for pid in self.prefabids:
            w.write_u64(pid)

        # Align to 4 for transforms
        w.align(4)

        # Write transforms
        for t in self.transforms:
            w.write_f32(t.rotation_x)
            w.write_f32(t.rotation_y)
            w.write_f32(t.rotation_z)
            w.write_f32(t.rotation_w)
            w.write_f32(t.position_x)
            w.write_f32(t.position_y)
            w.write_f32(t.position_z)
            w.write_f32(t.scale_x)
            w.write_f32(t.scale_y)
            w.write_f32(t.scale_z)
            w.write_u32(t.pad0)
            w.write_u32(t.pad1)

        # Write numpooled
        for np in self.numpooled:
            w.write_u16(np)

        # Align to 2 for parents
        w.align(2)

        # Write parents
        for p in self.parents:
            w.write_u16(p)

        # Align to 8 for bitfields
        w.align(8)

        # Write 5 bitfield arrays
        for i in range(5):
            for val in self.bits[i]:
                w.write_u64(val)

        return w.getvalue()

    def find_actor_index(self, actor_hash: int) -> int | None:
        """Find the index of an actor by its node ID hash."""
        for i, nid in enumerate(self.nodeids):
            if nid == actor_hash:
                return i
        return None

    def get_component_types_for_actor(self, actor_index: int) -> list[int]:
        """Get all component type hashes for a given actor index."""
        types = []
        for comp in self.components:
            if actor_index in comp.actor_indices:
                types.append(comp.type_hash)
        return types

    def clone_actor(self, source_index: int, new_node_hash: int, new_name_hash: int,
                    new_position: tuple[float, float, float] | None = None) -> int:
        """Clone an actor, appending to all parallel arrays. Returns new actor index."""
        new_index = self.actor_count

        # Add to headers (entity lookup table used by CRI binary search)
        # Headers are sorted by hash value (unsigned ascending, wrapping)
        # Insert at the correct sorted position
        import bisect
        header_hashes = [h.type_hash for h in self.headers]
        insert_pos = bisect.bisect_left(header_hashes, new_node_hash)
        self.headers.insert(insert_pos, ActorDataHeaderEntry(new_node_hash, new_index))

        # Clone nodeids, names, prefabids
        self.nodeids.append(new_node_hash)
        self.names.append(new_name_hash)
        self.prefabids.append(self.prefabids[source_index])

        # Clone transform with optional new position
        src_t = self.transforms[source_index]
        new_t = ActorDataTransform(
            src_t.rotation_x, src_t.rotation_y, src_t.rotation_z, src_t.rotation_w,
            new_position[0] if new_position else src_t.position_x,
            new_position[1] if new_position else src_t.position_y,
            new_position[2] if new_position else src_t.position_z,
            src_t.scale_x, src_t.scale_y, src_t.scale_z,
            src_t.pad0, src_t.pad1,
        )
        self.transforms.append(new_t)

        # Clone numpooled, parents
        self.numpooled.append(self.numpooled[source_index])
        self.parents.append(self.parents[source_index])

        # Clone bitfield membership
        old_qword_idx = source_index // 64
        old_bit_idx = source_index % 64
        new_qword_idx = new_index // 64
        new_bit_idx = new_index % 64

        for i in range(5):
            # Extend bitfield array if needed
            while len(self.bits[i]) <= new_qword_idx:
                self.bits[i].append(0)
            # Copy source bit to new position
            if old_qword_idx < len(self.bits[i]):
                bit_val = (self.bits[i][old_qword_idx] >> old_bit_idx) & 1
            else:
                bit_val = 0
            if bit_val:
                self.bits[i][new_qword_idx] |= (1 << new_bit_idx)

        # Add new index to component membership
        for comp in self.components:
            if source_index in comp.actor_indices:
                comp.actor_indices.append(new_index)

        return new_index
