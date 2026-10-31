#!/usr/bin/env python3
"""Pins the reference runtime's libr15 checksum list to the libr15 the APK carries (#1737).

WHY. The reference runtime (`lib/arm64-v8a/libvrapi.so` of the reference APK) holds twelve modules that act only on a
libr15 whose md5 is on the list each module carries (custctx, memsys, voipdrop and others). The libr15 this project's
APK carries is the reference file after the host patcher (quest_patch_libr15) and so has an md5 none of the lists
names; every one of those modules logs "OFF: libr15.so md5 ... is none of ..." and changes nothing. The md5 of the
patched file is a function of every input byte and of the patch table, so it is never a constant: the pin is computed
from the staged file on every build.

WHAT THE LIST IS IN THE BINARY. The compare is not a string compare against a text in .rodata. Each module's
`strcmp(hex, entry)` was compiled to four 64-bit XORs against constants built by `movz` + three `movk` instructions
(sixteen instruction words per module for one entry), so one entry of the list is written in two forms: twelve ASCII
copies (they feed the `%.8s` of each module's OFF line) and twelve sites of sixteen immediate words, one site per
module. Replacing the ASCII copies alone changes nothing the gate reads. The tool replaces both forms of one entry,
the entry at `--replace-index` of the table (the first list of consecutive 32-hex texts in the file; index 0
is the stock libr15's md5, which no build of this project carries), and keeps the other entries, so a libr15 that is the
reference's own is still accepted.

    quest_runtime_pin.py pin --libvrapi IN --libr15 FINAL_LIBR15 --out OUT [--replace-index 0]
    quest_runtime_pin.py detect --libvrapi IN

`pin` hashes FINAL_LIBR15 itself (the file the APK will carry), finds the twelve ASCII copies and the twelve sites (48
runs, 192 words) of the entry, asserts those exact counts, rewrites both forms in place (the size does not change and
only the imm16 field of each word and the 32 text bytes of each copy change), then re-reads the result and checks it:
the new text has twelve copies and twelve sites at that index, the old text none, and no byte outside the copies and
the imm16 fields differs. It is idempotent: the entry already holding the md5 is a no-op that writes a byte-identical
file. It refuses (exit 1) any other count, an md5 that is already present anywhere else in the file, a file that is
not an AArch64 ELF, and an OUT that is the input. `detect` says whether IN carries the table at all (exit 0) or is
some other libvrapi (exit 3: the store's own library carries none, and the recipe leaves it alone).
"""

from __future__ import annotations

import argparse
import hashlib
import json
import re
import struct
import sys
from pathlib import Path

TAG = "NEVR.QUEST.RUNTIME_PIN"
SITES = 12              # one per gated module
CHUNKS = 4              # 32 hex characters = four 8-byte compare chunks
WORDS_PER_RUN = 4       # movz + three movk
RUNS = SITES * CHUNKS   # 48
WORDS = RUNS * WORDS_PER_RUN  # 192
LOOKAHEAD = 64          # words after a movz searched for the three movk of one register
SITE_SPAN = 0x400       # the four runs of one site lie within this many bytes
EM_AARCH64 = 0xB7
PT_LOAD = 1
PF_X = 1
_HEX32 = re.compile(rb"(?<![0-9a-f])[0-9a-f]{32}(?=\0)")
_MD5_TEXT = re.compile(r"^[0-9a-f]{32}$")
_IMM_MASK = 0xFFFF << 5
_CHUNK = 1 << 20


class PinError(Exception):
    pass


def md5_of_file(path: Path) -> str:
    digest = hashlib.md5(usedforsecurity=False)
    with path.open("rb") as stream:
        while chunk := stream.read(_CHUNK):
            digest.update(chunk)
    return digest.hexdigest()


def sha256_hex(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def exec_ranges(data: bytes) -> list[tuple[int, int]]:
    """(file offset, size) of every executable PT_LOAD segment of a 64-bit little-endian AArch64 ELF."""
    if len(data) < 64 or data[:4] != b"\x7fELF" or data[4] != 2 or data[5] != 1:
        raise PinError("not a 64-bit little-endian ELF file")
    machine = struct.unpack_from("<H", data, 0x12)[0]
    if machine != EM_AARCH64:
        raise PinError(f"ELF machine 0x{machine:x} is not AArch64 (0x{EM_AARCH64:x})")
    phoff = struct.unpack_from("<Q", data, 0x20)[0]
    phentsize, phnum = struct.unpack_from("<HH", data, 0x36)
    if phentsize < 56 or phoff + phnum * phentsize > len(data):
        raise PinError("the program headers do not fit the file")
    found = []
    for k in range(phnum):
        p_type, flags, offset, _vaddr, _paddr, filesz = struct.unpack_from("<IIQQQQ", data, phoff + k * phentsize)
        if p_type == PT_LOAD and flags & PF_X:
            if offset + filesz > len(data):
                raise PinError("an executable segment runs past the end of the file")
            found.append((offset, filesz))
    if not found:
        raise PinError("the ELF file has no executable segment")
    return found


class Scan:
    """One pass over the file: every 64-bit `movz` (low half word) of the executable segments, indexed by its immediate."""

    def __init__(self, data: bytes):
        self.data = data
        self.words: list[tuple[int, list[int]]] = []  # (file offset of the first word, the words)
        for offset, size in exec_ranges(data):
            start = (offset + 3) & ~3
            count = (offset + size - start) // 4
            self.words.append((start, list(struct.unpack_from(f"<{count}I", data, start))))
        self.movz_low: dict[int, list[tuple[int, int]]] = {}  # imm16 -> [(segment index, word index)]
        for s, (_start, words) in enumerate(self.words):
            for i, w in enumerate(words):
                if (w & 0xFF800000) == 0xD2800000 and ((w >> 21) & 3) == 0:
                    self.movz_low.setdefault((w >> 5) & 0xFFFF, []).append((s, i))

    def runs(self, chunk: int) -> list[list[int]]:
        """The file offsets of the four words of every `movz` + three `movk` run that builds the 64-bit `chunk`."""
        halves = [(chunk >> (16 * k)) & 0xFFFF for k in range(4)]
        found: list[list[int]] = []
        for s, i in self.movz_low.get(halves[0], []):
            start, words = self.words[s]
            rd = words[i] & 31
            at = {0: i}
            for j in range(i + 1, min(i + 1 + LOOKAHEAD, len(words))):
                w = words[j]
                if (w & 31) != rd:
                    continue
                top = w & 0xFF800000
                if top == 0xD2800000 or top == 0x92800000:  # movz or movn: the register is rewritten
                    break
                if top == 0xF2800000:
                    hw = (w >> 21) & 3
                    if hw in at:
                        break
                    if ((w >> 5) & 0xFFFF) != halves[hw]:
                        break
                    at[hw] = j
                    if len(at) == 4:
                        break
            if len(at) == 4:
                found.append([start + 4 * at[k] for k in range(4)])
        return found


def ascii_copies(data: bytes, text: bytes) -> list[int]:
    """The offsets of every copy of `text` that is a whole NUL-terminated hex string."""
    found, at = [], 0
    while (at := data.find(text, at)) >= 0:
        before = data[at - 1:at]
        after = data[at + len(text):at + len(text) + 1]
        if after == b"\0" and not (before and before in b"0123456789abcdef"):
            found.append(at)
        at += 1
    return found


def chunks_of(text: bytes) -> list[int]:
    return [struct.unpack("<Q", text[i:i + 8])[0] for i in range(0, 32, 8)]


def carries_table(data: bytes) -> bool:
    """True when `data` holds a checksum table, whole or not: some 32-hex text has exactly SITES copies. A library that
    holds none is not the reference runtime (the store's own libvrapi.so is not)."""
    counts: dict[bytes, int] = {}
    for match in _HEX32.finditer(data):
        counts[match.group(0)] = counts.get(match.group(0), 0) + 1
    return SITES in counts.values()


def table(data: bytes) -> list[bytes]:
    """The table: the 32-hex texts of the first list in the file, each NUL-terminated and followed at once by the next
    (stride 33 bytes), starting at the lowest-offset hex text. Index 0 of the reference runtime is the stock libr15's md5."""
    first = _HEX32.search(data)
    if first is None:
        return []
    texts, at = [], first.start()
    while (match := _HEX32.match(data, at)) is not None:
        texts.append(match.group(0))
        at += 33
    return texts


def sites_of(scan: Scan, text: bytes) -> list[list[list[int]]]:
    """The runs of `text` grouped by site: a list of sites, each a list of four runs (one per chunk, in chunk order).

    PinError unless the runs are exactly SITES sites of one run per chunk within SITE_SPAN bytes."""
    per_chunk = [scan.runs(c) for c in chunks_of(text)]
    counts = [len(r) for r in per_chunk]
    if counts != [SITES] * CHUNKS:
        raise PinError(f"{text.decode()} has {sum(counts)} immediate runs ({counts} per chunk), not {RUNS} ({SITES} per chunk)")
    claimed: set[int] = set()
    for runs in per_chunk:
        for run in runs:
            if claimed & set(run):
                raise PinError(f"{text.decode()}: two runs share an instruction word")
            claimed |= set(run)
    sites = []
    firsts = sorted(per_chunk[0], key=lambda run: run[0])
    others = [sorted(r, key=lambda run: run[0]) for r in per_chunk[1:]]
    for n, first in enumerate(firsts):
        site = [first] + [o[n] for o in others]
        flat = [w for run in site for w in run]
        if max(flat) - min(flat) > SITE_SPAN:
            raise PinError(f"{text.decode()}: the runs of site {n} are {max(flat) - min(flat):#x} bytes apart, over {SITE_SPAN:#x}")
        sites.append(site)
    return sites


def check_md5(value: str) -> bytes:
    if not _MD5_TEXT.match(value):
        raise PinError(f"{value!r} is not 32 lowercase hex characters")
    return value.encode("ascii")


def pin_data(data: bytes, new_md5: str, replace_index: int = 0) -> tuple[bytes, dict]:
    """`data` with the table entry at `replace_index` replaced by `new_md5` in both forms; (result, report).

    PinError unless every rule in the module docstring holds."""
    new = check_md5(new_md5)
    if not carries_table(data):
        raise PinError("the file holds no checksum table (no 32-hex text with exactly "
                       f"{SITES} copies): it is not the reference runtime")
    entries = table(data)
    if not 0 <= replace_index < len(entries):
        raise PinError(f"--replace-index {replace_index}: the table has {len(entries)} entries")
    old = entries[replace_index]
    old_copies = ascii_copies(data, old)
    if len(old_copies) != SITES:
        raise PinError(f"{old.decode()} has {len(old_copies)} ASCII copies, not {SITES}")
    scan = Scan(data)
    old_sites = sites_of(scan, old)
    report = {"table": [text.decode() for text in entries], "replace_index": replace_index, "old_md5": old.decode(),
              "new_md5": new_md5, "ascii_copies": len(old_copies), "sites": len(old_sites),
              "immediate_runs": sum(len(site) for site in old_sites), "immediate_words": WORDS}
    if new == old:
        report["changed_bytes"] = 0
        return data, report
    new_copies = ascii_copies(data, new)
    new_runs = sum(len(scan.runs(c)) for c in chunks_of(new))
    if new_copies or new_runs:
        raise PinError(f"{new_md5} is already in the file ({len(new_copies)} ASCII copies, {new_runs} immediate runs) "
                       f"but is not the entry at index {replace_index}: refusing to make a second copy")
    out = bytearray(data)
    for offset in old_copies:
        out[offset:offset + 32] = new
    old_chunks, new_chunks = chunks_of(old), chunks_of(new)
    for site in old_sites:
        for k, run in enumerate(site):
            for hw, at in enumerate(run):
                w = struct.unpack_from("<I", out, at)[0]
                w = (w & ~_IMM_MASK) | (((new_chunks[k] >> (16 * hw)) & 0xFFFF) << 5)
                struct.pack_into("<I", out, at, w)
    result = bytes(out)
    verify_pinned(data, result, new, old, replace_index, old_copies, old_sites)
    report["changed_bytes"] = sum(1 for _ in zip_diff(data, result))
    return result, report


def zip_diff(a: bytes, b: bytes):
    """The (index, a byte, b byte) of every differing byte of two equal-length buffers; equal blocks are skipped fast."""
    for base in range(0, len(a), 4096):
        if a[base:base + 4096] != b[base:base + 4096]:
            for i in range(base, min(base + 4096, len(a))):
                if a[i] != b[i]:
                    yield i, a[i], b[i]


def verify_pinned(before: bytes, after: bytes, new: bytes, old: bytes, index: int, old_copies: list[int],
                  old_sites: list) -> None:
    """Re-decodes `after` and compares it with `before`; PinError on any difference from what a pin may change."""
    if len(after) != len(before):
        raise PinError("the pinned file is not the size of the input")
    entries, was = table(after), table(before)
    if len(entries) != len(was) or entries[index] != new or entries[:index] + entries[index + 1:] != was[:index] + was[index + 1:]:
        raise PinError(f"re-reading the result: the table is not the input's with entry {index} replaced by {new.decode()}")
    if ascii_copies(after, new) != old_copies:
        raise PinError("re-reading the result: the new entry's copies are not where the old ones were")
    scan = Scan(after)
    sites_of(scan, new)
    if ascii_copies(after, old) or any(scan.runs(c) for c in chunks_of(old)):
        raise PinError(f"re-reading the result: {old.decode()} is still in the file")
    allowed_ascii = [(o, o + 32) for o in old_copies]
    words = {at for site in old_sites for run in site for at in run}
    for i, a, b in zip_diff(before, after):
        if any(lo <= i < hi for lo, hi in allowed_ascii):
            continue
        word = i & ~3
        if word in words:
            wa = struct.unpack_from("<I", before, word)[0]
            wb = struct.unpack_from("<I", after, word)[0]
            if (wa ^ wb) & ~_IMM_MASK == 0:
                continue
        raise PinError(f"the pin changed byte {i:#x}, which is neither an ASCII copy nor the imm16 field of a pinned word")


def log(event: str, stream, **fields) -> None:
    print(json.dumps({"tag": TAG, "event": event, **fields}, sort_keys=True), file=stream)


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    sub = parser.add_subparsers(dest="cmd", required=True)
    pin = sub.add_parser("pin")
    pin.add_argument("--libvrapi", type=Path, required=True)
    pin.add_argument("--libr15", type=Path, required=True)
    pin.add_argument("--out", type=Path, required=True)
    pin.add_argument("--replace-index", type=int, default=0)
    detect = sub.add_parser("detect")
    detect.add_argument("--libvrapi", type=Path, required=True)
    args = parser.parse_args(argv)
    try:
        data = args.libvrapi.read_bytes()
        if args.cmd == "detect":
            exec_ranges(data)
            found = carries_table(data)
            log("runtime_detected" if found else "runtime_absent", sys.stdout, libvrapi_sha256=sha256_hex(data),
                table=[text.decode() for text in table(data)] if found else [])
            return 0 if found else 3
        if args.out.resolve() == args.libvrapi.resolve():
            raise PinError("--out is the input file: the pin writes a new file")
        md5 = md5_of_file(args.libr15)
        result, report = pin_data(data, md5, args.replace_index)
        args.out.write_bytes(result)
        log("runtime_pinned" if result != data else "runtime_pin_unchanged", sys.stdout, libr15_md5=md5,
            libvrapi_sha256_in=sha256_hex(data), libvrapi_sha256_out=sha256_hex(result), out=args.out.name, **report)
        return 0
    except (OSError, PinError) as error:
        log("runtime_pin_refused", sys.stderr, reason=str(error))
        return 1


if __name__ == "__main__":
    sys.exit(main())
