#!/usr/bin/env python3
"""List the Platform SDK imports a set of libpnsovr.so functions can reach by direct calls.

Used by `just test-quest-hooks-pinned` (#411): the login prerequisites answer four requests locally
by handing the game's message pump a synthetic message handle (src/quest/login/login_local_answers.h).
A synthetic handle must never reach a real SDK function, so every `ovr_*` import the pump and the four
login callbacks can reach is listed here and compared with the set the sentinel hooks or guards
(tools/pinned_ovr_imports.txt). A library that grows a new import on those paths fails the comparison.

Pure Python (no objdump): AArch64 ELF64, `.plt` stubs resolved through their GOT slot and `.rela.plt`.
Only direct `bl` / `b` edges are followed; the delegate table behind `CNSOVRMailbox::FulfillRequest`
is indirect, so the delegate proxies are roots as well.
"""

import argparse
import struct
import sys
from pathlib import Path

ELF_MAGIC = b"\x7fELF"
R_AARCH64_JUMP_SLOT = 1026


class Elf:
    def __init__(self, data: bytes):
        if data[:4] != ELF_MAGIC or data[4] != 2 or struct.unpack_from("<H", data, 0x12)[0] != 183:
            raise ValueError("not an AArch64 ELF64")
        self.data = data
        shoff, = struct.unpack_from("<Q", data, 0x28)
        shentsize, shnum, shstrndx = struct.unpack_from("<HHH", data, 0x3A)
        raw = []
        for i in range(shnum):
            raw.append(struct.unpack_from("<IIQQQQIIQQ", data, shoff + i * shentsize))
        names_off = raw[shstrndx][4]
        self.sections = {}
        for r in raw:
            end = data.index(b"\0", names_off + r[0])
            name = data[names_off + r[0]:end].decode()
            self.sections[name] = {"addr": r[3], "off": r[4], "size": r[5], "link": r[6], "entsize": r[9]}
        dynsym = self.sections[".dynsym"]
        dynstr = self.sections[".dynstr"]
        self.dynsyms = []
        for i in range(dynsym["size"] // 24):
            name, info, other, shndx, value, size = struct.unpack_from("<IBBHQQ", data, dynsym["off"] + i * 24)
            end = data.index(b"\0", dynstr["off"] + name)
            self.dynsyms.append((data[dynstr["off"] + name:end].decode(), value, size))
        rela = self.sections[".rela.plt"]
        self.got_to_symbol = {}
        for i in range(rela["size"] // 24):
            r_offset, r_info, _ = struct.unpack_from("<QQq", data, rela["off"] + i * 24)
            if (r_info & 0xFFFFFFFF) == R_AARCH64_JUMP_SLOT:
                self.got_to_symbol[r_offset] = self.dynsyms[r_info >> 32][0]
        self.plt_symbols = self._plt_stub_symbols()

    def word(self, address: int) -> int:
        for s in self.sections.values():
            if s["addr"] and s["addr"] <= address < s["addr"] + s["size"] and s["off"]:
                return struct.unpack_from("<I", self.data, s["off"] + address - s["addr"])[0]
        raise KeyError(hex(address))

    def _plt_stub_symbols(self) -> dict:
        """address of each PLT stub -> import name, from `adrp x16, page; ldr x17, [x16, #off]`."""
        plt = self.sections[".plt"]
        out = {}
        for addr in range(plt["addr"], plt["addr"] + plt["size"], 16):
            adrp = self.word(addr)
            ldr = self.word(addr + 4)
            if (adrp & 0x9F00001F) != 0x90000010 or (ldr & 0xFFC003FF) != 0xF9400211:
                continue
            immlo = (adrp >> 29) & 3
            immhi = (adrp >> 5) & 0x7FFFF
            imm = (immhi << 2) | immlo
            if imm & (1 << 20):
                imm -= 1 << 21
            page = ((addr >> 12) + imm) << 12
            got = page + (((ldr >> 10) & 0xFFF) << 3)
            if got in self.got_to_symbol:
                out[addr] = self.got_to_symbol[got]
        return out

    def symbol_address(self, name: str) -> int:
        for n, value, _ in self.dynsyms:
            if n == name and value:
                return value
        raise KeyError(name)


def walk(elf: Elf, roots: dict) -> dict:
    """import name -> sorted root names that reach it."""
    text = elf.sections[".text"]
    lo, hi = text["addr"], text["addr"] + text["size"]
    reached = {}
    seen = set()
    stack = [(name, addr, name) for name, addr in roots.items()]
    while stack:
        label, entry, origin = stack.pop()
        if entry in seen:
            continue
        seen.add(entry)
        a = entry
        furthest = entry
        while lo <= a < hi:
            w = elf.word(a)
            target = None
            call = False
            if (w & 0xFC000000) in (0x94000000, 0x14000000):  # bl / b
                imm = w & 0x3FFFFFF
                if imm & 0x2000000:
                    imm -= 1 << 26
                target = a + imm * 4
                call = (w & 0xFC000000) == 0x94000000
                if target in elf.plt_symbols:
                    reached.setdefault(elf.plt_symbols[target], set()).add(origin)
                    target = None
                elif lo <= target < hi and (call or target < entry or target > furthest + 0x10000):
                    stack.append((label, target, origin))
                    target = None
            elif (w & 0xFF000010) == 0x54000000 or (w & 0x7E000000) == 0x34000000 or (w & 0x7E000000) == 0x36000000:
                if (w & 0xFF000010) == 0x54000000 or (w & 0x7E000000) == 0x34000000:
                    imm = (w >> 5) & 0x7FFFF
                    if imm & 0x40000:
                        imm -= 1 << 19
                else:  # tbz / tbnz
                    imm = (w >> 5) & 0x3FFF
                    if imm & 0x2000:
                        imm -= 1 << 14
                target = a + imm * 4
            if target is not None and target > a:
                furthest = max(furthest, target)
            if w == 0xD65F03C0 and a >= furthest:  # ret
                break
            a += 4
    return {name: sorted(origins) for name, origins in reached.items()}


# The pump, the four callbacks, the mailbox dispatch and the two delegate proxies (link-time addresses of the
# pinned build; the callbacks are the GLOB_DAT-registered functions of login_prerequisite_targets.h).
ROOTS = {
    "Update": 0x207534,
    "GotLoggedInUserOrgIdCb": 0x1ECE60,
    "GotLoggedInUserCb": 0x1ECFE4,
    "GotLoggedInUserAccessTokenCb": 0x1ED1C0,
    "GotUserProofCB": 0x1ED578,
    "DelegateProxyA": 0x2089D0,
    "DelegateProxyB": 0x208BE0,
}
FULFILL = "_ZN10NRadEngine13CNSOVRMailbox14FulfillRequestEmP10ovrMessage"


def reachable_ovr_imports(path: Path) -> dict:
    elf = Elf(path.read_bytes())
    roots = dict(ROOTS)
    try:
        roots["FulfillRequest"] = elf.symbol_address(FULFILL)
    except KeyError:
        pass  # a local symbol in this build: the proxies and Update cover its callers
    imports = walk(elf, roots)
    return {name: origins for name, origins in imports.items() if name.startswith(("ovr_", "ovrID_"))}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("library", type=Path)
    parser.add_argument("--expect", type=Path, help="file of `import  # how the sentinel treats it` lines")
    args = parser.parse_args()
    found = reachable_ovr_imports(args.library)
    if args.expect is None:
        for name in sorted(found):
            print(name)
        return 0
    expected = set()
    for line in args.expect.read_text(encoding="utf-8").splitlines():
        line = line.split("#", 1)[0].strip()
        if line:
            expected.add(line.split()[0])
    missing = sorted(set(found) - expected)
    stale = sorted(expected - set(found))
    for name in missing:
        print(f"UNHANDLED: {name} is reachable from the pump or a login callback ({', '.join(found[name])}) and is "
              f"neither hooked nor guarded: a synthetic message handle could reach it", file=sys.stderr)
    for name in stale:
        print(f"STALE: {name} is listed in {args.expect} but no longer reachable (remove it)", file=sys.stderr)
    if missing or stale:
        return 1
    print(f"pinned_ovr_import_walk: {len(found)} ovr_* imports reachable, all accounted for")
    return 0


if __name__ == "__main__":
    sys.exit(main())
