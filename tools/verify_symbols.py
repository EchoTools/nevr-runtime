#!/usr/bin/env python3
"""A stripped PE and its split-off symbol file belong together.

The dist target splits the debug info off each shipped PE (objcopy --only-keep-debug), strips the PE and
adds a .gnu_debuglink section (objcopy --add-gnu-debuglink). That section holds the symbol file's name and
the CRC32 of its bytes: PE has no build-id, so name plus CRC32 is the link a debugger follows.

    verify_symbols.py <stripped.dll|exe> <file.dbg>
    verify_symbols.py --tree <package dir> <symbols dir>

--tree checks every .dll and .exe under the package directory (a dist package or the Windows zip's tree)
against <symbols dir>/<file name>.dbg; a PE with no symbol file there is a problem, and so is a package
with no PE at all. One name is exempt from needing a symbol file: nevr-install.exe, the copy of
install-nevr-runtime.exe that the release zip keeps for programs outside this repository that still call the old
name (#1492). It is the same bytes, so its debuglink names install-nevr-runtime.exe.dbg and the pair check on the new
name covers it; --tree requires the new name to be in the package and the two to be identical, and asks for no .dbg
of the old name.

Exits 0 when the PE carries no DWARF and no COFF symbol table, the .dbg carries DWARF and a non-empty COFF
symbol table (the function names a field crash maps to; a release build has no source lines), and the
debuglink names the .dbg with its CRC32. Exits 1 with one `verify_symbols: <file>: <reason>` line per
problem otherwise, including a file that is not a PE, is truncated, or is missing.
"""
from __future__ import annotations

import struct
import sys
import zlib
from pathlib import Path


class Pe:
    """The parts of a PE/COFF image the checks read: sections by name, and the COFF symbol table's extent."""

    def __init__(self, blob: bytes):
        self.size = len(blob)
        if blob[:2] != b"MZ":
            raise ValueError("not a PE file (no MZ header)")
        try:
            pe = struct.unpack_from("<I", blob, 0x3C)[0]
            if blob[pe:pe + 4] != b"PE\0\0":
                raise ValueError("not a PE file (no PE signature)")
            count, _, self.symtab, self.nsyms, optsize = struct.unpack_from("<HIIIH", blob, pe + 6)
            strtab = self.symtab + 18 * self.nsyms if self.symtab else 0
            table = pe + 24 + optsize
            self.sections: dict[str, bytes] = {}
            for n in range(count):
                raw, _, _, size, ptr = struct.unpack_from("<8sIIII", blob, table + 40 * n)
                name = raw.rstrip(b"\0").decode("ascii", "replace")
                if name.startswith("/") and strtab:
                    start = strtab + int(name[1:])
                    stop = blob.find(b"\0", start)
                    if stop < 0:
                        raise ValueError("truncated file (a section name runs past the end)")
                    name = blob[start:stop].decode("ascii", "replace")
                if ptr + size > len(blob):
                    raise ValueError(f"truncated file (section {name} runs {ptr + size - len(blob)} bytes past the end)")
                self.sections[name] = blob[ptr:ptr + size]
        except struct.error as err:
            raise ValueError("truncated file (the PE headers run past the end)") from err

    def has_dwarf(self) -> bool:
        return any(name.startswith(".debug_") for name in self.sections)

    def coff_symbols(self) -> str | None:
        """None when the COFF symbol table is present and inside the file, else why it is not."""
        if self.symtab == 0 or self.nsyms == 0:
            return "its COFF symbol table is empty (no function names)"
        if self.symtab + 18 * self.nsyms > self.size:
            return f"its COFF symbol table ({self.nsyms} symbols at {self.symtab:#x}) runs past the end of the file"
        return None


def load(path: Path) -> Pe:
    try:
        return Pe(path.read_bytes())
    except OSError as err:
        raise ValueError(f"cannot read: {err.strerror or err}") from err


def problems(binary: Path, symbols: Path) -> list[str]:
    """Every problem with the pair; a file that cannot be read as a PE is one line, never a traceback."""
    out = []
    pe = dbg = None
    for path, role in ((binary, "binary"), (symbols, "symbol file")):
        try:
            parsed = load(path)
        except ValueError as err:
            out.append(f"{path.name}: {err}")
            continue
        if role == "binary":
            pe = parsed
        else:
            dbg = parsed
    if pe is not None and pe.has_dwarf():
        out.append(f"{binary.name}: still carries DWARF sections (not stripped)")
    if pe is not None and pe.nsyms:
        out.append(f"{binary.name}: still carries a COFF symbol table ({pe.nsyms} symbols; not stripped)")
    if dbg is not None and not dbg.has_dwarf():
        out.append(f"{symbols.name}: carries no DWARF sections")
    if dbg is not None:
        why = dbg.coff_symbols()
        if why:
            out.append(f"{symbols.name}: {why}: a crash cannot be mapped to function names")
    if pe is None or dbg is None:
        return out
    link = pe.sections.get(".gnu_debuglink")
    if link is None:
        return out + [f"{binary.name}: no .gnu_debuglink section"]
    if b"\0" not in link or len(link) < ((link.index(b"\0") + 4) & ~3) + 4:
        return out + [f"{binary.name}: .gnu_debuglink section is too short to hold a name and a CRC32"]
    end = link.index(b"\0")
    name = link[:end].decode("ascii", "replace")
    crc = struct.unpack_from("<I", link, (end + 4) & ~3)[0]  # the CRC32 follows the name, 4-byte aligned
    if name != symbols.name:
        out.append(f"{binary.name}: debuglink names '{name}', not '{symbols.name}'")
    actual = zlib.crc32(symbols.read_bytes()) & 0xFFFFFFFF
    if crc != actual:
        out.append(f"{binary.name}: debuglink CRC32 {crc:#010x} != {symbols.name} CRC32 {actual:#010x}")
    return out


# A file in the package that is a copy of another, by name: the old name of the core installer (#1492). The copy has no
# symbol file of its own; it must be the same bytes as the original, which is checked on its own.
COPIES = {"nevr-install.exe": "install-nevr-runtime.exe"}


def tree_problems(package: Path, symbols_dir: Path) -> list[str]:
    files = sorted(p for p in package.rglob("*") if p.suffix.lower() in (".dll", ".exe"))
    if not files:
        return [f"{package}: no .dll or .exe to check"]
    out = []
    for pe in files:
        original = COPIES.get(pe.name)
        if original is not None and pe.parent == package:
            target = package / original
            if not target.is_file():
                out.append(f"{pe.relative_to(package)}: {original} is not in the package")
            elif pe.read_bytes() != target.read_bytes():
                out.append(f"{pe.relative_to(package)}: not the same bytes as {original}")
            continue
        dbg = symbols_dir / f"{pe.name}.dbg"
        if not dbg.is_file():
            out.append(f"{pe.relative_to(package)}: no symbol file {dbg}")
        else:
            out += problems(pe, dbg)
    return out


def main(argv: list[str]) -> int:
    if len(argv) == 4 and argv[1] == "--tree":
        found = tree_problems(Path(argv[2]), Path(argv[3]))
        for line in found:
            print(f"verify_symbols: {line}", file=sys.stderr)
        if not found:
            print(f"verify_symbols: every PE in {argv[2]} matches a symbol file in {argv[3]}")
        return 1 if found else 0
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
