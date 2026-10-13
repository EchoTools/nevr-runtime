#!/usr/bin/env python3
"""A stripped PE and its split-off symbol file belong together.

The dist target splits the debug info off each shipped PE (objcopy --only-keep-debug), strips the PE and
adds a .gnu_debuglink section (objcopy --add-gnu-debuglink). That section holds the symbol file's name and
the CRC32 of its bytes: PE has no build-id, so name plus CRC32 is the link a debugger follows.

    verify_symbols.py <stripped.dll|exe> <file.dbg>

Exits 0 when the PE carries no DWARF, the .dbg carries it, and the debuglink names the .dbg with its CRC32.
Exits 1 with one line per problem otherwise.
"""
from __future__ import annotations

import struct
import sys
import zlib
from pathlib import Path


def sections(blob: bytes) -> dict[str, bytes]:
    """name -> raw data of every section of a PE/COFF image (long names resolved via the string table)."""
    if blob[:2] != b"MZ":
        raise ValueError("not a PE file (no MZ header)")
    pe = struct.unpack_from("<I", blob, 0x3C)[0]
    if blob[pe:pe + 4] != b"PE\0\0":
        raise ValueError("not a PE file (no PE signature)")
    count, _, symtab, nsyms, optsize = struct.unpack_from("<HIIIH", blob, pe + 6)
    strtab = symtab + 18 * nsyms if symtab else 0
    table = pe + 24 + optsize
    found = {}
    for n in range(count):
        raw, _, _, size, ptr = struct.unpack_from("<8sIIII", blob, table + 40 * n)
        name = raw.rstrip(b"\0").decode("ascii", "replace")
        if name.startswith("/") and strtab:
            start = strtab + int(name[1:])
            name = blob[start:blob.index(b"\0", start)].decode("ascii", "replace")
        found[name] = blob[ptr:ptr + size]
    return found


def problems(binary: Path, symbols: Path) -> list[str]:
    out = []
    pe, dbg = sections(binary.read_bytes()), sections(symbols.read_bytes())
    if any(name.startswith(".debug_") for name in pe):
        out.append(f"{binary.name}: still carries DWARF sections (not stripped)")
    if not any(name.startswith(".debug_") for name in dbg):
        out.append(f"{symbols.name}: carries no DWARF sections")
    link = pe.get(".gnu_debuglink")
    if link is None:
        return out + [f"{binary.name}: no .gnu_debuglink section"]
    end = link.index(b"\0")
    name = link[:end].decode("ascii", "replace")
    crc = struct.unpack_from("<I", link, (end + 4) & ~3)[0]  # the CRC32 follows the name, 4-byte aligned
    if name != symbols.name:
        out.append(f"{binary.name}: debuglink names '{name}', not '{symbols.name}'")
    actual = zlib.crc32(symbols.read_bytes()) & 0xFFFFFFFF
    if crc != actual:
        out.append(f"{binary.name}: debuglink CRC32 {crc:#010x} != {symbols.name} CRC32 {actual:#010x}")
    return out


def main(argv: list[str]) -> int:
    if len(argv) != 3:
        print(__doc__, file=sys.stderr)
        return 2
    found = problems(Path(argv[1]), Path(argv[2]))
    for line in found:
        print(f"verify_symbols: {line}", file=sys.stderr)
    if not found:
        print(f"verify_symbols: {Path(argv[1]).name} matches {Path(argv[2]).name}")
    return 1 if found else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
