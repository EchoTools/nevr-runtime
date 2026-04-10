"""CTransformCR parser/serializer — 56-byte header + 176-byte entries."""

from __future__ import annotations
from dataclasses import dataclass
from ..binary_utils import BinaryReader, BinaryWriter
from ..descriptors import CRDescriptor
from .base import Resource

ENTRY_SIZE = 176


@dataclass
class TransformEntry:
    """176-byte CTransformCR entry."""
    node_hash: int              # +0x00
    entity_hash: int            # +0x08
    sentinel: int               # +0x10 (u32, always 0xFFFFFFFF)
    pad_14: int                 # +0x14
    component_flags: int        # +0x18
    rotation_x: float           # +0x20
    rotation_y: float           # +0x24
    rotation_z: float           # +0x28
    rotation_w: float           # +0x2C
    position_x: float           # +0x30
    position_y: float           # +0x34
    position_z: float           # +0x38
    scale_x: float              # +0x3C
    scale_y: float              # +0x40
    scale_z: float              # +0x44
    # Remaining 128 bytes stored as raw for round-trip fidelity
    tail_data: bytes            # +0x48 to +0xAF (128 bytes)

    STRUCT_PREFIX_SIZE = 0x48  # 72 bytes of typed fields
    TAIL_SIZE = ENTRY_SIZE - STRUCT_PREFIX_SIZE  # 104 bytes

    @classmethod
    def from_reader(cls, r: BinaryReader) -> TransformEntry:
        node_hash = r.read_u64()
        entity_hash = r.read_u64()
        sentinel = r.read_u32()
        pad_14 = r.read_u32()
        component_flags = r.read_u64()
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
        tail = r.read_bytes(cls.TAIL_SIZE)
        return cls(node_hash, entity_hash, sentinel, pad_14, component_flags,
                   rx, ry, rz, rw, px, py, pz, sx, sy, sz, tail)

    def to_writer(self, w: BinaryWriter) -> None:
        w.write_u64(self.node_hash)
        w.write_u64(self.entity_hash)
        w.write_u32(self.sentinel)
        w.write_u32(self.pad_14)
        w.write_u64(self.component_flags)
        w.write_f32(self.rotation_x)
        w.write_f32(self.rotation_y)
        w.write_f32(self.rotation_z)
        w.write_f32(self.rotation_w)
        w.write_f32(self.position_x)
        w.write_f32(self.position_y)
        w.write_f32(self.position_z)
        w.write_f32(self.scale_x)
        w.write_f32(self.scale_y)
        w.write_f32(self.scale_z)
        w.write_bytes(self.tail_data)

    def clone(self, new_entity_hash: int,
              position: tuple[float, float, float] | None = None,
              rotation: tuple[float, float, float, float] | None = None,
              ) -> TransformEntry:
        e = TransformEntry(
            node_hash=self.node_hash,       # Preserve original node_hash (e.g., ncaTransform for root)
            entity_hash=new_entity_hash,
            sentinel=self.sentinel,
            pad_14=self.pad_14,
            component_flags=self.component_flags,
            rotation_x=rotation[0] if rotation else self.rotation_x,
            rotation_y=rotation[1] if rotation else self.rotation_y,
            rotation_z=rotation[2] if rotation else self.rotation_z,
            rotation_w=rotation[3] if rotation else self.rotation_w,
            position_x=position[0] if position else self.position_x,
            position_y=position[1] if position else self.position_y,
            position_z=position[2] if position else self.position_z,
            scale_x=self.scale_x,
            scale_y=self.scale_y,
            scale_z=self.scale_z,
            tail_data=bytes(self.tail_data),
        )
        # Fix parent_hash in tail (offset 0x68 - 0x48 = 0x20 in tail)
        # and set node_hash = entity_hash for cloned actor
        return e


class TransformCR(Resource):
    """CTransformCR resource: 56-byte CRDescriptor + 176-byte entries."""

    type_name = "CTransformCRWin10"

    def __init__(self, descriptor: CRDescriptor, entries: list[TransformEntry]):
        self.descriptor = descriptor
        self.entries = entries

    @classmethod
    def from_bytes(cls, data: bytes) -> TransformCR:
        r = BinaryReader(data)
        desc = CRDescriptor.from_reader(r)
        entries = [TransformEntry.from_reader(r) for _ in range(desc.count)]
        return cls(desc, entries)

    def to_bytes(self) -> bytes:
        w = BinaryWriter()
        self.descriptor.update_for_count(len(self.entries), ENTRY_SIZE)
        self.descriptor.to_writer(w)
        for e in self.entries:
            e.to_writer(w)
        return w.getvalue()

    def find_by_entity(self, entity_hash: int) -> list[TransformEntry]:
        return [e for e in self.entries if e.entity_hash == entity_hash]

    def add_entry(self, entry: TransformEntry) -> None:
        self.entries.append(entry)
