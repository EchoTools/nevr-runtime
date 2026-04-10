#!/usr/bin/env python3
"""
RadEngine Archive Extract/Repack Tool
=======================================
Purely directory-driven. No metadata files except a tiny _version marker.

Extract: type_HASH/NAME.bin files
Repack:  scans directories, packs into ~512KB blocks, ZSTD compression

Usage:
  python rad_archive_tool.py extract <archive_dir> <output_dir>
  python rad_archive_tool.py repack  <input_dir> <output_archive_dir>
  python rad_archive_tool.py info    <archive_dir>

Requirements:
  pip install zstandard
"""
import argparse, json, os, re, struct, sys
from concurrent.futures import ThreadPoolExecutor, as_completed
from pathlib import Path

try:
    import zstandard
except ImportError:
    print("ERROR: pip install zstandard"); sys.exit(1)

# ---------------------------------------------------------------------------
# Constants
# ---------------------------------------------------------------------------
SEC0, SEC1, SEC2 = 32, 40, 16
HDR_SZ       = 0xC0
MAX_PKG      = 2_100_000_000
ZLVL         = 3
WLOG         = 19       # 512 KB window — matches engine expectation
W            = max(4, os.cpu_count() or 4)
BLOCK_TARGET = 524288   # 512 KB

_CPARAMS = zstandard.ZstdCompressionParameters(
    compression_level=ZLVL, window_log=WLOG, write_content_size=1)

# ---------------------------------------------------------------------------
# Hash lookup (name resolution)
# ---------------------------------------------------------------------------
# Inline radhash (CRC-64 with poly 0x95AC9329AC4BC9B5)
_POLY = 0x95AC9329AC4BC9B5; _M64 = 0xFFFFFFFFFFFFFFFF
_TBL = None
def _rad_hash(s):
    global _TBL
    if _TBL is None:
        _TBL = [0]*256
        for i in range(256):
            r = 0
            for b in range(7,-1,-1):
                if (i>>b)&1: r ^= _POLY
                r = (r<<1) & _M64
            _TBL[i] = r
    h = _M64
    for ch in s:
        c = ord(ch)
        if 0x41 <= c <= 0x5A: c += 0x20
        if c >= 0x80: c = c | (~0xFF & _M64)
        h = (c ^ _TBL[(h>>56)&0xFF]) ^ ((h<<8) & _M64)
    return h

_LOOKUP = None
_REVERSE = None  # name -> hash

def _load_lookup():
    global _LOOKUP, _REVERSE
    if _LOOKUP is not None: return
    lp = Path(__file__).parent / "hash_lookup.json"
    if lp.exists():
        raw = json.load(open(lp))
        _LOOKUP = {int(k, 16): v for k, v in raw.items()}
        _REVERSE = {v: int(k, 16) for k, v in raw.items()}
    else:
        _LOOKUP = {}
        _REVERSE = {}

def hash_to_name(h):
    """Convert a hash to a name. Returns '0xHASH' if unknown."""
    _load_lookup()
    name = _LOOKUP.get(h)
    if name: return name
    return f"0x{h:016X}"

def name_to_hash(name):
    """Convert a name back to hash. If starts with 0x, parse as hex. Otherwise hash it."""
    if name.startswith("0x") or name.startswith("0X"):
        try: return int(name, 16)
        except ValueError: pass
    _load_lookup()
    # Check reverse lookup first (exact match)
    if name in _REVERSE:
        return _REVERSE[name]
    # Hash it
    return _rad_hash(name)


def _compress(data: bytes) -> bytes:
    return zstandard.ZstdCompressor(compression_params=_CPARAMS).compress(data)


# ---------------------------------------------------------------------------
# I/O helpers
# ---------------------------------------------------------------------------
def read_mf(path):
    raw = Path(path).read_bytes()
    assert raw[:4] == b"ZSTD"
    return zstandard.ZstdDecompressor().decompress(raw[24:])


def write_mf(data, path, wflags=0x10):
    c = _compress(data)
    Path(path).write_bytes(
        struct.pack('<4sIQQ', b"ZSTD", 0x10, len(data), len(c)) + c)  # HeaderLength always 16


# ---------------------------------------------------------------------------
# Data structures
# ---------------------------------------------------------------------------
class Res:
    __slots__ = 'th','nh','bi','off','rsz','f1c','f1e'
    def __init__(s, th=0, nh=0, bi=0, off=0, rsz=0, f1c=0x10, f1e=0):
        s.th=th; s.nh=nh; s.bi=bi; s.off=off
        s.rsz=rsz; s.f1c=f1c; s.f1e=f1e
    @classmethod
    def read(cls, d, o=0):
        th, nh, bi, off = struct.unpack_from('<QQII', d, o)
        rsz, = struct.unpack_from('<I', d, o+24)
        f1c, f1e = struct.unpack_from('<HH', d, o+28)
        return cls(th, nh, bi, off, rsz, f1c, f1e)
    def write(s):
        return struct.pack('<QQIIIHH',
                           s.th, s.nh, s.bi, s.off, s.rsz, s.f1c, s.f1e)
    def key(s): return (s.th, s.nh)

class Info:
    __slots__ = 'th','nh','data'
    def __init__(s, th=0, nh=0, data=b'\0'*24):
        s.th=th; s.nh=nh; s.data=data
    @classmethod
    def read(cls, d, o=0):
        return cls(*struct.unpack_from('<QQ', d, o), d[o+16:o+40])
    def write(s):
        return struct.pack('<QQ', s.th, s.nh) + s.data
    def key(s): return (s.th, s.nh)

class Blk:
    __slots__ = 'pi','off','csz','dsz'
    def __init__(s, pi=0, off=0, csz=0, dsz=0):
        s.pi=pi; s.off=off; s.csz=csz; s.dsz=dsz
    @classmethod
    def read(cls, d, o=0):
        return cls(*struct.unpack_from('<IIII', d, o))
    def write(s):
        return struct.pack('<IIII', s.pi, s.off, s.csz, s.dsz)
    def has_data(s): return s.csz > 0 and s.dsz > 0

class Manifest:
    def __init__(s):
        s.version = 1; s.flags = 0x00080000
        s.res = []; s.infos = []; s.blks = []
    @staticmethod
    def parse(data):
        m = Manifest()
        m.version = struct.unpack_from('<I', data, 0)[0]
        m.flags   = struct.unpack_from('<I', data, 4)[0]
        n0 = struct.unpack_from('<Q', data, 0x30)[0]; n1 = n0
        s0e = HDR_SZ + n0*SEC0; s1e = s0e + n1*SEC1
        n2 = (len(data) - s1e) // SEC2
        for i in range(n0): m.res.append(Res.read(data, HDR_SZ+i*SEC0))
        for i in range(n1): m.infos.append(Info.read(data, s0e+i*SEC1))
        for i in range(n2): m.blks.append(Blk.read(data, s1e+i*SEC2))
        return m
    def serialize(s):
        parts = [build_header(len(s.res), len(s.infos), len(s.blks),
                              s.version, s.flags)]
        for r in s.res:   parts.append(r.write())
        for i in s.infos: parts.append(i.write())
        for b in s.blks:  parts.append(b.write())
        return b''.join(parts)

def build_header(n0, n1, n2, version=1, flags=0):
    """Build manifest header matching original game format.
    - PackageCount at offset 0, Unk1 (flags) at offset 4
    - Section.Unk2 = 0x100000000 for all sections (game value)
    - ElementSize = 32 for all sections (game value, regardless of actual entry size)
    """
    h = bytearray(HDR_SZ)
    struct.pack_into('<II', h, 0, version, flags)  # PackageCount, Unk1
    # FrameContents section (0x10)
    struct.pack_into('<Q',   h, 0x10, n0*SEC0)             # Length
    struct.pack_into('<Q',   h, 0x20, 0x100000000)         # Unk2
    struct.pack_into('<QQQ', h, 0x28, SEC0, n0, n0)        # ElementSize=32, Count, ElementCount
    # Metadata section (0x50)
    struct.pack_into('<Q',   h, 0x50, n1*SEC1)             # Length
    struct.pack_into('<Q',   h, 0x60, 0x100000000)         # Unk2
    struct.pack_into('<QQQ', h, 0x68, SEC0, n1, n1)        # ElementSize=32 (game uses 32 for all)
    # Frames section (0x90)
    struct.pack_into('<Q',   h, 0x90, n2*SEC2)             # Length
    struct.pack_into('<Q',   h, 0xA0, 0x100000000)         # Unk2
    struct.pack_into('<QQQ', h, 0xA8, SEC0, n2, n2)        # ElementSize=32 (game uses 32 for all)
    return bytes(h)

def find_archives(d):
    d = Path(d); out = {}
    for md in d.rglob("manifests"):
        pd = md.parent / "packages"
        if not pd.exists(): continue
        for f in sorted(md.iterdir()):
            if f.is_file():
                out[f.name] = {'mf': f, 'pkgs': sorted(pd.glob(f"{f.name}_*"))}
    return out

# ---------------------------------------------------------------------------
# EXTRACT  (fast: mmap packages, parallel decompress + parallel writes)
# ---------------------------------------------------------------------------
def extract_archive(archive_dir, output_dir):
    output = Path(output_dir)
    for mf_hash, paths in find_archives(archive_dir).items():
        m = Manifest.parse(read_mf(str(paths['mf'])))
        print(f"\n=== {mf_hash}: {len(m.res)} res, v{m.version} ===")

        # Open packages as plain file handles (no mmap — portable across all machines)
        pkg_fds = {}
        for p in paths['pkgs']:
            idx = int(p.name.rsplit('_', 1)[1])
            pkg_fds[idx] = open(p, 'rb')

        # Thread lock for file seeks (file handles aren't thread-safe)
        import threading
        pkg_locks = {idx: threading.Lock() for idx in pkg_fds}

        def _read_pkg(pi, offset, size):
            """Thread-safe read from a package file."""
            lock = pkg_locks.get(pi)
            fd = pkg_fds.get(pi)
            if not lock or not fd:
                return None
            with lock:
                fd.seek(offset)
                return fd.read(size)

        bb = {}
        for r in m.res: bb.setdefault(r.bi, []).append(r)

        # Pre-create all type directories (using resolved names)
        mf_out = output / mf_hash
        type_dirs = set()
        for r in m.res:
            type_dirs.add(r.th)
        for th in type_dirs:
            dirname = hash_to_name(th)
            (mf_out / dirname).mkdir(parents=True, exist_ok=True)

        # Pre-sort resources per block once
        bb_sorted = {}
        for bi, rl in bb.items():
            sr = sorted(rl, key=lambda r: r.off)
            bb_sorted[bi] = sr

        data_blks = [(i, b) for i, b in enumerate(m.blks) if b.has_data()]
        cnt = 0
        err = 0
        BATCH = min(2000, len(data_blks))

        def _decompress_and_split(bi, blk):
            """Read, decompress a block, and split into resources."""
            comp = _read_pkg(blk.pi, blk.off, blk.csz)
            if comp is None or len(comp) != blk.csz:
                print(f"  WARNING: block {bi} read failed (pkg={blk.pi} off={blk.off} csz={blk.csz}, got {len(comp) if comp else 0})")
                return []
            try:
                raw = zstandard.ZstdDecompressor().decompress(
                    comp, max_output_size=blk.dsz + 65536)
            except Exception as e:
                print(f"  WARNING: block {bi} decompress failed (pkg={blk.pi} off={blk.off} csz={blk.csz} dsz={blk.dsz}): {e}")
                return []
            rl = bb_sorted.get(bi, [])
            results = []
            for j, r in enumerate(rl):
                tname = hash_to_name(r.th)
                nname = hash_to_name(r.nh)
                results.append((tname, nname, raw[r.off:r.off + r.rsz]))
            return results

        for bs in range(0, len(data_blks), BATCH):
            batch = data_blks[bs:bs + BATCH]

            # Parallel decompress + split
            all_files = []
            with ThreadPoolExecutor(W) as pool:
                fs = {pool.submit(_decompress_and_split, bi, blk): idx
                      for idx, (bi, blk) in enumerate(batch)}
                for f in as_completed(fs):
                    res = f.result()
                    if not res:
                        err += 1
                    all_files.extend(res)

            # Parallel file writes
            def _write(args):
                tname, nname, data = args
                (mf_out / tname / f"{nname}.bin").write_bytes(data)
            with ThreadPoolExecutor(W) as pool:
                list(pool.map(_write, all_files))

            cnt += len(all_files)
            del all_files

            if bs + BATCH >= len(data_blks) or (bs // BATCH) % 2 == 1:
                print(f"    {min(bs+BATCH, len(data_blks))}/{len(data_blks)} blocks...")

        for fd in pkg_fds.values(): fd.close()
        (mf_out / "_version").write_text(f"{m.version}\n{m.flags}\n")
        print(f"  Extracted {cnt} resources -> {mf_out}")
        if err:
            print(f"  WARNING: {err} blocks failed to extract!")

# ---------------------------------------------------------------------------
# REPACK  (memory-efficient: batched block processing)
# ---------------------------------------------------------------------------
def repack_archive(input_dir, output_dir, manifest_hash=None):
    input_dir  = Path(input_dir)
    output_dir = Path(output_dir)
    mf_dirs = [d for d in sorted(input_dir.iterdir())
               if d.is_dir() and any(
                   sd.is_dir() and not sd.name.startswith("_")
                   for sd in d.iterdir())]
    if not mf_dirs: print("ERROR: No resource dirs found."); return

    for mf_dir in mf_dirs:
        mf_name = manifest_hash or mf_dir.name
        print(f"\n=== Repacking: {mf_name} ===")

        version, flags = 3, 0
        vf = mf_dir / "_version"
        if vf.exists():
            lines = vf.read_text().strip().split('\n')
            version = int(lines[0])
            if len(lines) > 1: flags = int(lines[1])
        wflags = 0x10  # HeaderLength always 16 (reference format)

        # Scan files (paths + sizes only)
        # Directory names: either "0xHASH" or resolved names
        # File names: either "0xHASH.bin" or "resolved_name.bin"
        files = []
        for td in sorted(mf_dir.iterdir()):
            if not td.is_dir() or td.name.startswith("_"): continue
            th = name_to_hash(td.name)
            for f in sorted(td.iterdir()):
                if f.is_file() and f.name.endswith(".bin"):
                    nh = name_to_hash(f.stem)
                    files.append((th, nh, f, f.stat().st_size))

        print(f"  {len(files)} resources (v{version})")
        if not files: continue

        # Separate zero-size resources — they MUST NOT be in blocks > 512KB
        # (engine streaming decompressor fails with "buffer too small")
        nonzero_files = [(th, nh, fp, sz) for th, nh, fp, sz in files if sz > 0]
        zero_files    = [(th, nh, fp, sz) for th, nh, fp, sz in files if sz == 0]

        # Plan blocks from non-zero files only
        block_plan = []
        cur_block = []; cur_sz = 0
        for th, nh, fp, sz in nonzero_files:
            if cur_block and cur_sz + sz > BLOCK_TARGET:
                block_plan.append(cur_block)
                cur_block = []; cur_sz = 0
            cur_block.append((th, nh, fp, sz))
            cur_sz += sz
        if cur_block: block_plan.append(cur_block)

        # Attach zero-size resources to the first small block (< 512KB)
        # at offset = block_decompressed_size
        zero_block_idx = 0  # default: attach to block 0
        for i, blk_files in enumerate(block_plan):
            blk_sz = sum(s for _, _, _, s in blk_files)
            if blk_sz <= BLOCK_TARGET:
                zero_block_idx = i
                break
        # Append zero-size entries to the chosen block
        if zero_files:
            block_plan[zero_block_idx].extend(zero_files)

        print(f"  {len(block_plan)} blocks ({len(zero_files)} zero-size resources -> block {zero_block_idx})")

        # Process in large batches — use available RAM aggressively
        # ~4000 blocks × 512KB ≈ 2GB peak (raw) + compressed ≈ 3GB total
        BATCH = min(max(W * 64, 1024), 4000)
        res_entries = []; info_entries = []; blk_entries = []

        platform = output_dir / "rad15" / "win10"
        (platform / "manifests").mkdir(parents=True, exist_ok=True)
        (platform / "packages").mkdir(parents=True, exist_ok=True)

        pkg_idx = 0; pkg_off = 0; pkg_sizes = []
        pkg_file = open(platform / "packages" / f"{mf_name}_{pkg_idx}", 'wb')
        total = len(block_plan); done = 0

        def _read_and_build_block(block_files):
            """Read files for one block and return (raw_bytes, entries)."""
            raw = bytearray(); entries = []
            for th, nh, fp, sz in block_files:
                off = len(raw)
                raw.extend(fp.read_bytes())
                entries.append((th, nh, off, sz))
            return (bytes(raw), entries)

        def _read_and_compress(block_files):
            """Read + compress a single block in one shot."""
            raw = bytearray(); entries = []
            for th, nh, fp, sz in block_files:
                off = len(raw)
                raw.extend(fp.read_bytes())
                entries.append((th, nh, off, sz))
            raw = bytes(raw)
            comp = _compress(raw)
            return (raw, comp, entries)

        for bs in range(0, total, BATCH):
            batch = block_plan[bs:bs+BATCH]

            # Parallel read + compress (I/O and CPU overlap across threads)
            results = [None] * len(batch)
            with ThreadPoolExecutor(W) as pool:
                fs = {pool.submit(_read_and_compress, bf): i
                      for i, bf in enumerate(batch)}
                for f in as_completed(fs):
                    results[fs[f]] = f.result()

            # Write to package
            for raw, comp, entries in results:
                if pkg_off + len(comp) > MAX_PKG and pkg_off > 0:
                    pkg_file.close()
                    pkg_sizes.append(pkg_off)
                    pkg_idx += 1; pkg_off = 0
                    pkg_file = open(
                        platform / "packages" / f"{mf_name}_{pkg_idx}", 'wb')

                bi = len(blk_entries)
                blk_entries.append(Blk(pkg_idx, pkg_off, len(comp), len(raw)))
                for th, nh, off, sz in entries:
                    res_entries.append(Res(th, nh, bi, off, sz))
                    info_entries.append(Info(th, nh))
                pkg_file.write(comp)
                pkg_off += len(comp)

            del results
            done += len(batch)
            if done % 1000 < BATCH or done == total:
                print(f"    {done}/{total} blocks...")

        pkg_file.close()
        pkg_sizes.append(pkg_off)

        # Sort by signed (type, name) — engine binary-searches the manifest
        import ctypes
        paired = list(zip(res_entries, info_entries))
        paired.sort(key=lambda p: (ctypes.c_int64(p[0].th).value,
                                   ctypes.c_int64(p[0].nh).value))
        res_entries  = [p[0] for p in paired]
        info_entries = [p[1] for p in paired]

        # End markers + terminator
        all_blks = list(blk_entries)
        for pi, sz in enumerate(pkg_sizes):
            all_blks.append(Blk(pi, sz, 0, 0))
        all_blks.append(Blk(0, 0, 0, 0))

        mn = Manifest()
        mn.version = len(pkg_sizes); mn.flags = flags
        mn.res = res_entries; mn.infos = info_entries; mn.blks = all_blks
        write_mf(mn.serialize(), str(platform / "manifests" / mf_name), wflags)

        nd = sum(1 for b in all_blks if b.has_data())
        mfsz = (platform / "manifests" / mf_name).stat().st_size
        for pi, sz in enumerate(pkg_sizes):
            print(f"  Package {pi}: {sz:,} bytes")
        print(f"  Manifest: {mfsz:,} bytes")
        print(f"  {len(res_entries)} res, {nd} data blks, {len(pkg_sizes)} pkgs")

# ---------------------------------------------------------------------------
# INFO
# ---------------------------------------------------------------------------
def show_info(archive_dir):
    for h, p in find_archives(archive_dir).items():
        m = Manifest.parse(read_mf(str(p['mf'])))
        nd = sum(1 for b in m.blks if b.has_data())
        print(f"\n=== {h} (v{m.version}) ===")
        print(f"  {len(m.res)} res  {nd} blks  {len(p['pkgs'])} pkgs")
        tc = {}
        for r in m.res: tc[r.th] = tc.get(r.th, 0) + 1
        for th, c in sorted(tc.items(), key=lambda x: -x[1])[:15]:
            print(f"    {th:016X}: {c}")

# ---------------------------------------------------------------------------
def main():
    p = argparse.ArgumentParser(description="RadEngine Archive Extract/Repack Tool")
    sp = p.add_subparsers(dest='cmd')
    ex = sp.add_parser('extract'); ex.add_argument('archive_dir'); ex.add_argument('output_dir')
    rp = sp.add_parser('repack');  rp.add_argument('input_dir');  rp.add_argument('output_dir')
    rp.add_argument('--manifest-hash', default=None)
    sp.add_parser('info').add_argument('archive_dir')
    args = p.parse_args()
    if args.cmd == 'extract':   extract_archive(args.archive_dir, args.output_dir)
    elif args.cmd == 'repack':  repack_archive(args.input_dir, args.output_dir,
                                               getattr(args, 'manifest_hash', None))
    elif args.cmd == 'info':    show_info(args.archive_dir)
    else: p.print_help()

if __name__ == '__main__': main()
