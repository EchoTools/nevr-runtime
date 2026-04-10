"""CRDescriptor and RadArrayDescriptor types for NRadEngine binary formats."""

from __future__ import annotations
from dataclasses import dataclass
from .binary_utils import BinaryReader, BinaryWriter


@dataclass
class CRDescriptor:
    """56-byte header shared by all Component Resource (CR) files.

    On disk: padding0=0, mem_context=0, padding1=0, flags=1 (version),
    alloc_base=0, count=capacity, data_size=count*entry_size.
    """
    padding0: int = 0
    data_size: int = 0
    mem_context: int = 0
    padding1: int = 0
    flags: int = 1
    alloc_base: int = 0
    count: int = 0
    capacity: int = 0

    SIZE = 56

    @classmethod
    def from_reader(cls, r: BinaryReader) -> CRDescriptor:
        return cls(
            padding0=r.read_u64(),
            data_size=r.read_u64(),
            mem_context=r.read_u64(),
            padding1=r.read_u32(),
            flags=r.read_u32(),
            alloc_base=r.read_u64(),
            count=r.read_u64(),
            capacity=r.read_u64(),
        )

    def to_writer(self, w: BinaryWriter) -> None:
        w.write_u64(self.padding0)
        w.write_u64(self.data_size)
        w.write_u64(self.mem_context)
        w.write_u32(self.padding1)
        w.write_u32(self.flags)
        w.write_u64(self.alloc_base)
        w.write_u64(self.count)
        w.write_u64(self.capacity)

    @property
    def entry_size(self) -> int:
        if self.count == 0:
            return 0
        return self.data_size // self.count

    def update_for_count(self, count: int, entry_size: int) -> None:
        self.count = count
        self.capacity = count
        self.data_size = count * entry_size


@dataclass
class RadArrayDescriptor56:
    """56-byte array descriptor used throughout NRadEngine serialization.

    On disk: pData=0, pAllocator=0, pBase=0, flags=1.
    dataByteSize = count * element_size.
    """
    p_data: int = 0
    data_byte_size: int = 0
    p_allocator: int = 0
    pad0: int = 0
    flags: int = 1
    p_base: int = 0
    capacity: int = 0
    count: int = 0

    SIZE = 56

    @classmethod
    def from_reader(cls, r: BinaryReader) -> RadArrayDescriptor56:
        return cls(
            p_data=r.read_u64(),
            data_byte_size=r.read_u64(),
            p_allocator=r.read_u64(),
            pad0=r.read_u32(),
            flags=r.read_u32(),
            p_base=r.read_u64(),
            capacity=r.read_u64(),
            count=r.read_u64(),
        )

    def to_writer(self, w: BinaryWriter) -> None:
        w.write_u64(self.p_data)
        w.write_u64(self.data_byte_size)
        w.write_u64(self.p_allocator)
        w.write_u32(self.pad0)
        w.write_u32(self.flags)
        w.write_u64(self.p_base)
        w.write_u64(self.capacity)
        w.write_u64(self.count)

    @property
    def element_size(self) -> int:
        if self.count == 0:
            return 0
        return self.data_byte_size // self.count

    def update_for_count(self, count: int, elem_size: int) -> None:
        self.count = count
        self.capacity = count
        self.data_byte_size = count * elem_size


@dataclass
class RadArrayDescriptor64(RadArrayDescriptor56):
    """64-byte array descriptor with an extra field (bitCount for bitfield arrays)."""
    extra_field: int = 0

    SIZE = 64

    @classmethod
    def from_reader(cls, r: BinaryReader) -> RadArrayDescriptor64:
        base = RadArrayDescriptor56.from_reader(r)
        extra = r.read_u64()
        return cls(
            p_data=base.p_data,
            data_byte_size=base.data_byte_size,
            p_allocator=base.p_allocator,
            pad0=base.pad0,
            flags=base.flags,
            p_base=base.p_base,
            capacity=base.capacity,
            count=base.count,
            extra_field=extra,
        )

    def to_writer(self, w: BinaryWriter) -> None:
        super().to_writer(w)
        w.write_u64(self.extra_field)
