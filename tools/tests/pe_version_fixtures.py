"""Hand-built PE files that carry the pinned VERSIONINFO, with no toolchain (#850).

pe_with_resources / resource_tree / version_block are the bricks; shipped_pe is BugSplat64.dll or
echovr_server.exe as tools/pe_version_info.py pins them, with `payload` after the image. The real-compiler
oracle for the reader is in test_pe_version_info.py (windres); these files let a test that only needs a file
the gate accepts, such as test_package_release.py, build one without it."""
from __future__ import annotations

import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import pe_version_info as vi  # noqa: E402


def pe_with_resources(rsrc: bytes) -> bytes:
    """A minimal PE32+ whose only section is .rsrc, holding `rsrc` at RVA 0x1000, raw data at 0x200."""
    dos = b"MZ" + b"\0" * 0x3A + struct.pack("<I", 0x40)
    coff = struct.pack("<HHIIIHH", 0x8664, 1, 0, 0, 0, 240, 0x2022)
    optional = bytearray(240)
    struct.pack_into("<H", optional, 0, 0x20B)
    struct.pack_into("<I", optional, 108, 16)
    struct.pack_into("<II", optional, 112 + 2 * 8, 0x1000, len(rsrc))
    section = struct.pack("<8sIIIIIIHHI", b".rsrc", len(rsrc), 0x1000, len(rsrc), 0x200, 0, 0, 0, 0, 0)
    header = dos + b"PE\0\0" + coff + bytes(optional) + section
    return header.ljust(0x200, b"\0") + rsrc


def resource_tree(blob: bytes) -> bytes:
    """type 16 -> name 1 -> language 0x409 -> one data entry for `blob`, as a .rsrc section body at RVA 0x1000."""
    top = struct.pack("<IIHHHH", 0, 0, 0, 0, 0, 1) + struct.pack("<II", 16, 0x80000018)
    name = struct.pack("<IIHHHH", 0, 0, 0, 0, 0, 1) + struct.pack("<II", 1, 0x80000030)
    lang = struct.pack("<IIHHHH", 0, 0, 0, 0, 0, 1) + struct.pack("<II", 0x409, 0x48)
    entry = struct.pack("<IIII", 0x1000 + 0x58, len(blob), 0, 0)
    return top + name + lang + entry + blob


def version_block(key: str, value: bytes, children: bytes = b"", kind: int = 0, value_length: int | None = None) -> bytes:
    head = key.encode("utf-16-le") + b"\0\0"
    body = struct.pack("<HHH", 0, len(value) if value_length is None else value_length, kind) + head
    body += b"\0" * (-len(body) % 4) + value
    body += b"\0" * (-len(body) % 4) + children
    return struct.pack("<H", len(body)) + body[2:]


def join_blocks(blocks: list) -> bytes:
    """Sibling version blocks, each starting on a 4-byte boundary."""
    out = b""
    for block in blocks:
        out += b"\0" * (-len(out) % 4) + block
    return out


def string_block(name: str, value: str) -> bytes:
    return version_block(name, (value + "\0").encode("utf-16-le"), kind=1, value_length=len(value) + 1)


def version_info_blob(file_name: str, version: str) -> bytes:
    """The VS_VERSIONINFO block for `file_name` at `version`, every field as the pins say."""
    numeric = vi.numeric_version(version)
    spec = vi.EXPECTED_FILES[file_name]
    fixed = struct.pack("<13I", vi.FIXED_SIGNATURE, 0x10000, numeric[0] << 16 | numeric[1], numeric[2] << 16 | numeric[3],
                        numeric[0] << 16 | numeric[1], numeric[2] << 16 | numeric[3], 0x3F, 0, vi.VOS_NT_WINDOWS32,
                        spec["file_type"], vi.VFT2_UNKNOWN, 0, 0)
    strings = join_blocks([string_block(name, value) for name, value in sorted(vi.expected_strings(file_name, version).items())])
    table = version_block(vi.TABLE_KEY, b"", strings)
    translation = version_block("Translation", struct.pack("<HH", *vi.TRANSLATION))
    children = join_blocks([version_block("StringFileInfo", b"", table), version_block("VarFileInfo", b"", translation)])
    return version_block("VS_VERSION_INFO", fixed, children)


def shipped_pe(file_name: str, version: str, payload: bytes = b"") -> bytes:
    """A PE whose resources are the one VERSIONINFO block of `file_name` at `version`, then `payload`."""
    return pe_with_resources(resource_tree(version_info_blob(file_name, version))) + payload
