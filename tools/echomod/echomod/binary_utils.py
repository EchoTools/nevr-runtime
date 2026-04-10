"""Low-level binary read/write helpers for little-endian data."""

import struct
import io
from typing import Any


class BinaryReader:
    """Stateful little-endian binary reader wrapping a bytes buffer."""

    def __init__(self, data: bytes):
        self._stream = io.BytesIO(data)
        self._size = len(data)

    def tell(self) -> int:
        return self._stream.tell()

    def seek(self, pos: int) -> None:
        self._stream.seek(pos)

    def remaining(self) -> int:
        return self._size - self._stream.tell()

    def read_bytes(self, n: int) -> bytes:
        data = self._stream.read(n)
        if len(data) != n:
            raise EOFError(f"Expected {n} bytes at offset {self.tell() - len(data)}, got {len(data)}")
        return data

    def read_u8(self) -> int:
        return struct.unpack("<B", self.read_bytes(1))[0]

    def read_u16(self) -> int:
        return struct.unpack("<H", self.read_bytes(2))[0]

    def read_u32(self) -> int:
        return struct.unpack("<I", self.read_bytes(4))[0]

    def read_u64(self) -> int:
        return struct.unpack("<Q", self.read_bytes(8))[0]

    def read_i16(self) -> int:
        return struct.unpack("<h", self.read_bytes(2))[0]

    def read_i32(self) -> int:
        return struct.unpack("<i", self.read_bytes(4))[0]

    def read_i64(self) -> int:
        return struct.unpack("<q", self.read_bytes(8))[0]

    def read_f32(self) -> float:
        return struct.unpack("<f", self.read_bytes(4))[0]

    def read_f64(self) -> float:
        return struct.unpack("<d", self.read_bytes(8))[0]

    def read_struct(self, fmt: str) -> tuple:
        size = struct.calcsize(fmt)
        return struct.unpack(fmt, self.read_bytes(size))

    def align(self, alignment: int) -> int:
        """Advance to the next aligned position. Returns bytes skipped."""
        pos = self.tell()
        remainder = pos % alignment
        if remainder:
            skip = alignment - remainder
            self.read_bytes(skip)
            return skip
        return 0


class BinaryWriter:
    """Stateful little-endian binary writer."""

    def __init__(self):
        self._stream = io.BytesIO()

    def tell(self) -> int:
        return self._stream.tell()

    def seek(self, pos: int) -> None:
        self._stream.seek(pos)

    def write_bytes(self, data: bytes) -> None:
        self._stream.write(data)

    def write_u8(self, val: int) -> None:
        self._stream.write(struct.pack("<B", val))

    def write_u16(self, val: int) -> None:
        self._stream.write(struct.pack("<H", val))

    def write_u32(self, val: int) -> None:
        self._stream.write(struct.pack("<I", val))

    def write_u64(self, val: int) -> None:
        self._stream.write(struct.pack("<Q", val))

    def write_i16(self, val: int) -> None:
        self._stream.write(struct.pack("<h", val))

    def write_i32(self, val: int) -> None:
        self._stream.write(struct.pack("<i", val))

    def write_i64(self, val: int) -> None:
        self._stream.write(struct.pack("<q", val))

    def write_f32(self, val: float) -> None:
        self._stream.write(struct.pack("<f", val))

    def write_f64(self, val: float) -> None:
        self._stream.write(struct.pack("<d", val))

    def write_struct(self, fmt: str, *args: Any) -> None:
        self._stream.write(struct.pack(fmt, *args))

    def write_pad(self, n: int, byte: int = 0) -> None:
        self._stream.write(bytes([byte]) * n)

    def align(self, alignment: int, byte: int = 0) -> int:
        """Pad to the next aligned position. Returns bytes written."""
        pos = self.tell()
        remainder = pos % alignment
        if remainder:
            pad = alignment - remainder
            self.write_pad(pad, byte)
            return pad
        return 0

    def getvalue(self) -> bytes:
        return self._stream.getvalue()
