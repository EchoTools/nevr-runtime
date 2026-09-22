#!/usr/bin/env python3
"""Resolve thread stacks in a Windows minidump to module+offset (needs `pip install minidump`).

    tools/winvm/dump_stacks.py hang.dmp [--frames 12]

Produced by `systest.py --dump-on-fail` (comsvcs.dll MiniDump, full memory).
Prints, per thread, the RIP and the return-address-looking values on the stack,
with an echovr.exe VA (image base 0x140000000) ready to paste into ReVault.

Read the output with care: there is no unwind info here, so the walk is a scan
for values that point into a module right after a CALL instruction. The TOP few
frames are reliable. Deeper ones are frequently STALE stack data left by earlier
calls, and will happily "resolve" to a plausible function that is not running.
Corroborate anything you act on (e.g. dump the guest's windows: a thread parked
in user32 is usually a dialog, not a network call).
"""

from __future__ import annotations

import argparse
import struct
import sys

from minidump.minidumpfile import MinidumpFile

ECHOVR_IMAGE_BASE = 0x140000000


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("dump")
    ap.add_argument("--frames", type=int, default=12, help="max frames per thread")
    args = ap.parse_args()

    mf = MinidumpFile.parse(args.dump)
    modules = sorted(((m.baseaddress, m.size, m.name.split("\\")[-1]) for m in mf.modules.modules))
    segments = [(s.start_virtual_address, s.end_virtual_address, s.start_file_address)
                for s in mf.memory_segments_64.memory_segments]

    def resolve(addr: int):
        for base, size, name in modules:
            if base <= addr < base + size:
                return name, addr - base
        return None

    with open(args.dump, "rb") as raw:
        def read(addr: int, n: int) -> bytes:
            for start, end, offset in segments:
                if start <= addr < end:
                    raw.seek(offset + (addr - start))
                    return raw.read(min(n, end - addr))
            return b""

        def after_call(addr: int):
            hit = resolve(addr)
            pre = read(addr - 7, 7)
            if not hit or len(pre) < 7:
                return None
            is_call = (pre[2] == 0xE8 or pre[1:3] == b"\xff\x15"
                       or (pre[5] == 0xFF and 0xD0 <= pre[6] <= 0xD7)
                       or (pre[4] == 0xFF and 0x50 <= pre[5] <= 0x57)
                       or (pre[1] == 0xFF and 0x90 <= pre[2] <= 0x97))
            return hit if is_call else None

        for t in mf.threads.threads:
            rip, rsp = t.ContextObject.Rip, t.ContextObject.Rsp
            print(f"\n=== thread {t.ThreadId}  rip={resolve(rip)}")
            stack = read(rsp, 0x3000)
            shown = 0
            for i in range(0, len(stack) - 7, 8):
                hit = after_call(struct.unpack_from("<Q", stack, i)[0])
                if not hit:
                    continue
                va = f"  (echovr VA 0x{ECHOVR_IMAGE_BASE + hit[1]:x})" if hit[0].lower() == "echovr.exe" else ""
                print(f"  [rsp+0x{i:04x}] {hit[0]}+0x{hit[1]:x}{va}")
                shown += 1
                if shown >= args.frames:
                    break
    return 0


if __name__ == "__main__":
    sys.exit(main())
