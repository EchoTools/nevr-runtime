#!/usr/bin/env python3
"""List a PE DLL's export surface and group exports that share one address (#20).

The game reaches pnsrad.dll / pnsovr.dll / pnsdemo.dll only through GetProcAddress-style lookups by
name, so the export table IS the platform-service interface. Identical-code folding makes several
exports share one body (pnsrad's Mic* exports are one 3-byte `return 0`), which a per-export hook
cannot tell apart; this tool reports those groups, forwarders and trivial stubs up front.

    python3 tools/pe_exports.py pnsrad.dll [pnsovr.dll ...]            # table per DLL
    python3 tools/pe_exports.py --compare pnsrad.dll pnsovr.dll         # which names only one DLL exports
    python3 tools/pe_exports.py --json pnsrad.dll

Read-only: the file is parsed with struct, never loaded or executed.
"""
from __future__ import annotations

import argparse
import json
import struct
import sys
from collections import defaultdict
from dataclasses import dataclass, asdict
from pathlib import Path

# x64 bodies that do nothing: `ret`; `xor eax,eax; ret` (both encodings); `mov eax,0; ret`.
TRIVIAL_STUBS = {
    bytes.fromhex("c3"): "ret",
    bytes.fromhex("c20000"): "ret",          # `ret 0`, the same no-op
    bytes.fromhex("31c0c3"): "return 0",
    bytes.fromhex("33c0c3"): "return 0",
    bytes.fromhex("b800000000c3"): "return 0",
    bytes.fromhex("b801000000c3"): "return 1",
}


class PeError(ValueError):
    pass


@dataclass
class Export:
    name: str
    ordinal: int
    rva: int
    forwarder: str | None
    group: int          # number of exports (by name) sharing this RVA
    head: str           # first 16 bytes of the body, hex ("" for a forwarder)
    stub: str | None    # "return 0" / "ret" / ... when the body is one of TRIVIAL_STUBS


def _sections(data: bytes, pe: int):
    (number_of_sections,) = struct.unpack_from("<H", data, pe + 6)
    (opt_size,) = struct.unpack_from("<H", data, pe + 20)
    base = pe + 24 + opt_size
    for i in range(number_of_sections):
        vsize, va, rawsize, rawptr = struct.unpack_from("<IIII", data, base + i * 40 + 8)
        yield va, max(vsize, rawsize), rawptr, rawsize


def _rva_to_off(data: bytes, pe: int, rva: int) -> int:
    for va, size, raw, rawsize in _sections(data, pe):
        if va <= rva < va + size:
            off = raw + (rva - va)
            if off >= len(data):
                raise PeError(f"RVA 0x{rva:x} maps past the end of the file")
            return off
    raise PeError(f"RVA 0x{rva:x} is in no section")


def _cstr(data: bytes, off: int) -> str:
    end = data.index(b"\0", off)
    return data[off:end].decode("ascii", "replace")


def parse_exports(data: bytes) -> list[Export]:
    if data[:2] != b"MZ":
        raise PeError("not a PE file (no MZ)")
    (pe,) = struct.unpack_from("<I", data, 0x3C)
    if data[pe:pe + 4] != b"PE\0\0":
        raise PeError("not a PE file (no PE signature)")
    (magic,) = struct.unpack_from("<H", data, pe + 24)
    if magic != 0x20B:
        raise PeError("only PE32+ (x64) images are supported")
    export_rva, export_size = struct.unpack_from("<II", data, pe + 24 + 112)
    if export_rva == 0:
        return []
    off = _rva_to_off(data, pe, export_rva)
    # IMAGE_EXPORT_DIRECTORY: Characteristics, TimeDateStamp, Major, Minor, Name, Base,
    # NumberOfFunctions, NumberOfNames, AddressOfFunctions, AddressOfNames, AddressOfNameOrdinals.
    (_, _, _, _, _, ordinal_base, n_funcs, n_names, addr_funcs, addr_names, addr_ords) = struct.unpack_from(
        "<IIHHIIIIIII", data, off)
    funcs_off = _rva_to_off(data, pe, addr_funcs)
    names_off = _rva_to_off(data, pe, addr_names)
    ords_off = _rva_to_off(data, pe, addr_ords)
    rvas = struct.unpack_from(f"<{n_funcs}I", data, funcs_off)

    raw: list[tuple[str, int, int]] = []
    for i in range(n_names):
        (name_rva,) = struct.unpack_from("<I", data, names_off + 4 * i)
        (index,) = struct.unpack_from("<H", data, ords_off + 2 * i)
        raw.append((_cstr(data, _rva_to_off(data, pe, name_rva)), ordinal_base + index, rvas[index]))

    by_rva: dict[int, int] = defaultdict(int)
    for _, _, rva in raw:
        by_rva[rva] += 1

    exports = []
    for name, ordinal, rva in sorted(raw):
        forwarder = None
        head = ""
        stub = None
        if export_rva <= rva < export_rva + export_size:        # a forwarder string lives inside the table
            forwarder = _cstr(data, _rva_to_off(data, pe, rva))
        else:
            start = _rva_to_off(data, pe, rva)
            body = data[start:start + 16]
            head = body.hex()
            for pattern, label in TRIVIAL_STUBS.items():
                if body.startswith(pattern):
                    stub = label
        exports.append(Export(name, ordinal, rva, forwarder, by_rva[rva], head, stub))
    return exports


def folded_groups(exports: list[Export]) -> dict[int, list[str]]:
    groups: dict[int, list[str]] = defaultdict(list)
    for e in exports:
        if e.forwarder is None:
            groups[e.rva].append(e.name)
    return {rva: names for rva, names in groups.items() if len(names) > 1}


def compare(a: list[Export], b: list[Export]) -> tuple[list[str], list[str], list[str]]:
    na, nb = {e.name for e in a}, {e.name for e in b}
    return sorted(na - nb), sorted(nb - na), sorted(na & nb)


def render_table(label: str, exports: list[Export]) -> str:
    lines = [f"{label}: {len(exports)} named exports"]
    lines.append(f"{'name':<34} {'ord':>4} {'rva':>10}  fold  body")
    for e in exports:
        body = f"-> {e.forwarder}" if e.forwarder else (e.stub or e.head[:24])
        lines.append(f"{e.name:<34} {e.ordinal:>4} 0x{e.rva:08x}  {e.group:>4}  {body}")
    groups = folded_groups(exports)
    if groups:
        lines.append("shared bodies (identical-code folding or one stub):")
        for rva, names in sorted(groups.items()):
            lines.append(f"  0x{rva:08x}: {', '.join(names)}")
    return "\n".join(lines)


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("dll", nargs="+", type=Path)
    ap.add_argument("--json", action="store_true", help="machine-readable output")
    ap.add_argument("--compare", action="store_true", help="two DLLs: names only one of them exports")
    args = ap.parse_args(argv)
    try:
        tables = [(p, parse_exports(p.read_bytes())) for p in args.dll]
    except (OSError, PeError) as err:
        print(f"pe_exports: {err}", file=sys.stderr)
        return 2
    if args.compare:
        if len(tables) != 2:
            print("pe_exports: --compare takes exactly two DLLs", file=sys.stderr)
            return 2
        (pa, ea), (pb, eb) = tables
        only_a, only_b, both = compare(ea, eb)
        print(f"only in {pa.name} ({len(only_a)}): {', '.join(only_a) or '-'}")
        print(f"only in {pb.name} ({len(only_b)}): {', '.join(only_b) or '-'}")
        print(f"in both ({len(both)}): {', '.join(both) or '-'}")
        return 0
    if args.json:
        print(json.dumps({p.name: [asdict(e) for e in ex] for p, ex in tables}, indent=1))
        return 0
    print("\n\n".join(render_table(p.name, ex) for p, ex in tables))
    return 0


if __name__ == "__main__":
    sys.exit(main())
