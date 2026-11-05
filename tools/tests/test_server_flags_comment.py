"""#455: the comments on the SERVER_FLAGS_CHECK patch must state the bits its bytes set.

The patch site is echovr.exe VA 0x1401580C3 (RVA 0x1580C3), 0x113 bytes into
fcn.140157fb0 (disassembly 0x140157fb0). Its 40 bytes are OR QWORD PTR [RAX], 0x6
followed by NOPs. The stock bytes at that site test bits 1, 2 and 6 of the qword
that CR15NetGame+0x2DA0 points to; the stock bytes below are a ReVault read of
echovr.exe, and the decode of their three TEST instructions gives those bits.

The test reads the comment in src/runtime/hook/addresses.h and the comment in
PatchEnableServer in src/runtime/patch/mode_patches.cpp, and checks each claim
against the bytes.
"""

import re
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
ADDRESSES = ROOT / "src/runtime/hook/addresses.h"
SOURCE = ROOT / "src/runtime/patch/mode_patches.cpp"

ADDRESSES_BLOCK_START = "// Server Mode Patches (PatchEnableServer)"
ADDRESSES_CONST = "constexpr uintptr_t SERVER_FLAGS_CHECK ="
PATCH_FUNCTION = "VOID PatchEnableServer() {"
PATCH_APPLY = "ApplyPatch(SERVER_FLAGS_CHECK, serverFlagsCheck"

# ReVault read of echovr.exe, 0x1401580C3..0x1401580EA, before the patch.
STOCK = bytes.fromhex(
    "48 8b 08 0f b6 c1 d0 e8 41 84 c6 0f 85 de 00 00 00 "
    "0f b6 c1 c0 e8 02 41 84 c6 75 0c c0 e9 06 41 84 ce 0f 85 c7 00 00 00"
)
# Each stock shift, followed by TEST R14B (R14D is 1 from 0x14015806F), names the bit it tests.
SHIFT_BIT = {"d0 e8": 1, "c0 e8 02": 2, "c0 e9 06": 6}
SETS_CLAIM = re.compile(r"\bsets bits? ((?:\d+)(?:(?:,\s*|\s+and\s+)\d+)*)")
TESTS_CLAIM = re.compile(r"\btests? bits? ((?:\d+)(?:(?:,\s*|\s+and\s+)\d+)*)")
OR_PREFIX = bytes([0x48, 0x83, 0x08])  # OR QWORD PTR [RAX], imm8


def bits_of(mask: int) -> set[int]:
    return {bit for bit in range(8) if mask >> bit & 1}


def numbers(group: str) -> set[int]:
    return {int(number) for number in re.findall(r"\d+", group)}


def addresses_block(text: str) -> str:
    start = text.index(ADDRESSES_BLOCK_START)
    return text[start : text.index(ADDRESSES_CONST, start)]


def patch_block(text: str) -> str:
    start = text.index(PATCH_FUNCTION)
    return text[start : text.index(PATCH_APPLY, start)]


def patch_bytes(text: str) -> bytes:
    body = re.search(r"serverFlagsCheck\[\]\s*=\s*\{(.*?)\};", text, re.DOTALL).group(1)
    body = re.sub(r"//[^\n]*", "", body)
    return bytes(int(value, 16) for value in re.findall(r"0x([0-9A-Fa-f]{2})\b", body))


def size_constant(text: str) -> int:
    return int(re.search(r"SERVER_FLAGS_CHECK_SIZE\s*=\s*(\d+)", text).group(1))


def stock_tested_bits(stock: bytes) -> set[int]:
    text = " ".join(f"{byte:02x}" for byte in stock)
    return {
        SHIFT_BIT[match.group(1)]
        for match in re.finditer(r"(d0 e8|c0 e8 02|c0 e9 06) 41 84 c[6e]", text)
    }


def problems(addresses_text: str, source_text: str) -> list[str]:
    found: list[str] = []
    block = addresses_block(addresses_text)
    comment_in_patch = patch_block(source_text)
    data = patch_bytes(source_text)
    size = size_constant(addresses_text)

    if len(data) != size:
        found.append(f"patch is {len(data)} bytes; SERVER_FLAGS_CHECK_SIZE is {size}")
    if data[:4] != OR_PREFIX + bytes([0x06]):
        found.append(f"patch starts {data[:4].hex(' ')}; expected 48 83 08 06 (OR QWORD PTR [RAX], 0x6)")
    if any(byte != 0x90 for byte in data[4:]):
        found.append("patch bytes after the OR are not all NOPs")
    or_bits = bits_of(data[3]) if data[:3] == OR_PREFIX else set()

    for label, text in (("addresses.h", block), ("mode_patches.cpp", comment_in_patch)):
        claims = [numbers(match) for match in SETS_CLAIM.findall(text)]
        if len(claims) != 1:
            found.append(f"{label}: expected one statement that the patch sets bits; found {len(claims)}")
        elif claims[0] != or_bits:
            found.append(
                f"{label}: says the patch sets bits {sorted(claims[0])}; "
                f"OR mask 0x06 sets bits {sorted(or_bits)}"
            )

    if re.search(r"\bforces?\b", block, re.IGNORECASE):
        found.append("addresses.h: the comment says the patch forces bits; it sets bits and removes stock tests")

    stock_bits = stock_tested_bits(STOCK)
    reads = [numbers(match) for match in TESTS_CLAIM.findall(block)]
    if reads != [stock_bits]:
        found.append(
            f"addresses.h: says the stock code tests bits {[sorted(r) for r in reads]}; "
            f"the pinned stock bytes test bits {sorted(stock_bits)}"
        )

    for needle in ("fcn.140157fb0", "0x140157fb0", "0x1401580C3"):
        if needle not in block:
            found.append(f"addresses.h: the comment does not cite {needle} (ADR 0035 P2)")
    return found


class ServerFlagsComment(unittest.TestCase):
    def setUp(self):
        self.addresses = ADDRESSES.read_text(encoding="utf-8")
        self.source = SOURCE.read_text(encoding="utf-8")

    def test_the_shipped_comments_match_the_patch_bytes_and_the_stock_bytes(self):
        self.assertEqual(problems(self.addresses, self.source), [])

    def test_the_pinned_stock_bytes_are_forty_and_test_bits_one_two_and_six(self):
        self.assertEqual(len(STOCK), 40)
        self.assertEqual(stock_tested_bits(STOCK), {1, 2, 6})

    def test_an_or_mask_that_sets_another_bit_is_caught(self):
        self.assertIn("0x48, 0x83, 0x08, 0x06,", self.source)
        mutated = self.source.replace("0x48, 0x83, 0x08, 0x06,", "0x48, 0x83, 0x08, 0x0E,", 1)
        self.assertTrue(problems(self.addresses, mutated))

    def test_a_patch_comment_that_names_another_bit_is_caught(self):
        self.assertIn("sets bits 1 and 2", self.source)
        mutated = self.source.replace("sets bits 1 and 2", "sets bits 1 and 3", 1)
        self.assertTrue(problems(self.addresses, mutated))


if __name__ == "__main__":
    unittest.main()
