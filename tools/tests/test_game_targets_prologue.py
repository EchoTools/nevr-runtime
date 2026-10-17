"""The host pins the first bytes of echovr.exe's GameMainWrapper (src/runtime/host/game_targets.h).

Nothing else checks those bytes against the game: the host's own tests only compare them with
themselves. With the game binary present this reads the bytes at the wrapper's RVA and compares
them; without it, the comparison tests skip and the parsing tests still run on a synthetic PE.
The binary is read only, from this checkout's echovr/ or the main checkout's (a worktree has none).
"""

import re
import struct
import subprocess
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
HEADER = REPO / "src" / "runtime" / "host" / "game_targets.h"
GAME_RELATIVE = Path("echovr") / "bin" / "win10" / "echovr.exe"


def parse_targets(text):
    """(rva, prologue bytes) from game_targets.h."""
    rva = int(re.search(r"kGameMainWrapperRva\s*=\s*(0x[0-9A-Fa-f]+)", text).group(1), 16)
    body = re.search(r"kGameMainWrapperPrologue\[[^\]]*\]\s*=\s*\{([^}]*)\}", text, re.S).group(1)
    data = bytes(int(token, 16) for token in re.findall(r"0x[0-9A-Fa-f]{2}", body))
    size = int(re.search(r"kGameMainWrapperPrologueSize\s*=\s*(\d+)", text).group(1))
    return rva, data, size


def read_at_rva(pe, rva, length):
    """Bytes at `rva` in a PE32+ image file, through its section table."""
    if pe[:2] != b"MZ":
        raise ValueError("not a PE file")
    nt = struct.unpack_from("<I", pe, 0x3C)[0]
    if pe[nt:nt + 4] != b"PE\0\0":
        raise ValueError("no PE signature")
    sections = struct.unpack_from("<H", pe, nt + 6)[0]
    optional_size = struct.unpack_from("<H", pe, nt + 20)[0]
    table = nt + 24 + optional_size
    for index in range(sections):
        name, virtual_size, virtual_address, raw_size, raw_pointer = struct.unpack_from(
            "<8sIIII", pe, table + 40 * index)
        if virtual_address <= rva and rva + length <= virtual_address + max(virtual_size, raw_size):
            offset = raw_pointer + (rva - virtual_address)
            return pe[offset:offset + length]
    raise ValueError(f"rva 0x{rva:x} is in no section")


def find_game_exe():
    candidates = [REPO / GAME_RELATIVE]
    common = subprocess.run(["git", "-C", str(REPO), "rev-parse", "--git-common-dir"],
                            capture_output=True, text=True).stdout.strip()
    if common:
        candidates.append((REPO / common).resolve().parent / GAME_RELATIVE)
    for candidate in candidates:
        if candidate.is_file():
            return candidate
    return None


def synthetic_pe(rva, content):
    """A minimal PE32+ with one section holding `content` at `rva`."""
    raw_pointer = 0x400
    pe = bytearray(raw_pointer + 0x1000)
    pe[0:2] = b"MZ"
    struct.pack_into("<I", pe, 0x3C, 0x80)
    pe[0x80:0x84] = b"PE\0\0"
    struct.pack_into("<HH", pe, 0x84, 0x8664, 1)  # machine, sections
    struct.pack_into("<H", pe, 0x80 + 20, 0xF0)   # optional header size
    table = 0x80 + 24 + 0xF0
    section_rva = rva & ~0xFFF
    struct.pack_into("<8sIIII", pe, table, b".text", 0x1000, section_rva, 0x1000, raw_pointer)
    start = raw_pointer + (rva - section_rva)
    pe[start:start + len(content)] = content
    return bytes(pe)


class GameTargetsPrologueTest(unittest.TestCase):
    def setUp(self):
        self.rva, self.prologue, self.size = parse_targets(HEADER.read_text(encoding="utf-8"))

    def test_header_is_the_wrapper_and_thirteen_bytes(self):
        self.assertEqual(self.rva, 0xCD510)
        self.assertEqual(self.size, 13)
        self.assertEqual(len(self.prologue), self.size)

    def test_rva_reading_follows_the_section_table(self):
        pe = synthetic_pe(self.rva, self.prologue)
        self.assertEqual(read_at_rva(pe, self.rva, self.size), self.prologue)
        with self.assertRaises(ValueError):
            read_at_rva(pe, 0x5000000, 4)

    def test_a_changed_byte_in_the_header_is_not_the_synthetic_image(self):
        pe = synthetic_pe(self.rva, self.prologue)
        changed = bytearray(self.prologue)
        changed[7] ^= 0x01
        self.assertNotEqual(read_at_rva(pe, self.rva, self.size), bytes(changed))

    def test_prologue_matches_the_game_binary(self):
        exe = find_game_exe()
        if exe is None:
            self.skipTest("echovr.exe is not present (this checkout and the main checkout were searched)")
        found = read_at_rva(exe.read_bytes(), self.rva, self.size)
        self.assertEqual(found, self.prologue,
                         f"game_targets.h pins {self.prologue.hex()} but {exe} has {found.hex()} at 0x{self.rva:x}")


if __name__ == "__main__":
    unittest.main()
