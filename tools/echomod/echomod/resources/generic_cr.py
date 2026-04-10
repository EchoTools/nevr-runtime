"""Generic Component Resource parser for simple CR types with fixed-size entries.

Handles any CR type that has a 56-byte CRDescriptor header followed by
count * entry_size bytes of entry data, with optional trailing data.

Entry header (common to all CRs):
  +0x00: u64 type_hash
  +0x08: u64 actor_hash
  +0x10: u16 node_index (0xFFFF = unresolved)
  +0x12: u16 padding
"""

from __future__ import annotations
from dataclasses import dataclass, field
from ..binary_utils import BinaryReader, BinaryWriter
from ..descriptors import CRDescriptor
from .base import Resource


@dataclass
class CREntry:
    """A single entry in a generic CR file."""
    type_hash: int          # +0x00
    actor_hash: int         # +0x08
    node_index: int         # +0x10 (u16)
    node_pad: int           # +0x12 (u16)
    component_data: bytes   # +0x14 onwards (entry_size - 20 bytes)

    HEADER_SIZE = 20

    @classmethod
    def from_reader(cls, r: BinaryReader, entry_size: int) -> CREntry:
        type_hash = r.read_u64()
        actor_hash = r.read_u64()
        node_index = r.read_u16()
        node_pad = r.read_u16()
        data_size = entry_size - cls.HEADER_SIZE
        component_data = r.read_bytes(data_size) if data_size > 0 else b""
        return cls(type_hash, actor_hash, node_index, node_pad, component_data)

    def to_writer(self, w: BinaryWriter) -> None:
        w.write_u64(self.type_hash)
        w.write_u64(self.actor_hash)
        w.write_u16(self.node_index)
        w.write_u16(self.node_pad)
        if self.component_data:
            w.write_bytes(self.component_data)

    def clone(self, new_actor_hash: int, new_node_hash: int | None = None,
              node_index: int = 0xFFFF) -> CREntry:
        return CREntry(
            type_hash=new_node_hash if new_node_hash is not None else self.type_hash,
            actor_hash=new_actor_hash,
            node_index=node_index,
            node_pad=self.node_pad,
            component_data=bytes(self.component_data),
        )


class GenericCR(Resource):
    """Generic Component Resource: 56-byte header + fixed-size entries + optional tail."""

    def __init__(self, descriptor: CRDescriptor, entries: list[CREntry],
                 entry_size: int, tail: bytes = b""):
        self.descriptor = descriptor
        self.entries = entries
        self.entry_size = entry_size
        self.tail = tail

    @classmethod
    def from_bytes(cls, data: bytes) -> GenericCR:
        r = BinaryReader(data)
        desc = CRDescriptor.from_reader(r)
        entry_size = desc.entry_size
        if entry_size == 0 and desc.count == 0:
            # Empty resource — try to detect entry size from file structure
            entry_size = 0
        entries = []
        for _ in range(desc.count):
            entries.append(CREntry.from_reader(r, entry_size))
        tail = r.read_bytes(r.remaining()) if r.remaining() > 0 else b""
        return cls(desc, entries, entry_size, tail)

    def to_bytes(self) -> bytes:
        w = BinaryWriter()
        # Only update descriptor counts if entries changed
        if len(self.entries) != self.descriptor.count:
            self.descriptor.count = len(self.entries)
            self.descriptor.capacity = len(self.entries)
            self.descriptor.data_size = len(self.entries) * self.entry_size
        self.descriptor.to_writer(w)
        for entry in self.entries:
            entry.to_writer(w)
        if self.tail:
            w.write_bytes(self.tail)
        return w.getvalue()

    def find_entries_by_actor(self, actor_hash: int) -> list[CREntry]:
        return [e for e in self.entries if e.actor_hash == actor_hash]

    def add_entry(self, entry: CREntry) -> None:
        self.entries.append(entry)
