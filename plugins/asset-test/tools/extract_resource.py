#!/usr/bin/env python3
"""Extract one resource's bytes from the game's own package files, by (type, name).

Used to build the asset-test plugin's payload from a local install, so the repository never carries game
data. Format: docs/reference/package-manifest-format.md. Needs the `zstd` command line tool.

  extract_resource.py --data DIR --manifest-id HEX --type CGameLevelResourceWin10 --name mpl_tutorial_master \
      --out level.bin
  extract_resource.py ... --c-array kLevel      # print a C array instead of writing --out

DIR is the platform data directory that holds manifests/ and packages/
(e.g. <game>/_data/5932408047/rad15/win10). --type and --name take a symbol name or 0x<16 hex digits>.
"""
import argparse
import importlib.util
import os
import struct
import subprocess
import sys


def load_hash(repo_root):
    spec = importlib.util.spec_from_file_location(
        "gen_symbol_corpus", os.path.join(repo_root, "tools", "gen_symbol_corpus.py"))
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod.csymbol64_hash


def zstd_decompress(data):
    out = subprocess.run(["zstd", "-d", "-c", "-q"], input=data, stdout=subprocess.PIPE, check=True)
    return out.stdout


def read_manifest(path):
    raw = open(path, "rb").read()
    if raw[:4] != b"ZSTD" or struct.unpack_from("<I", raw, 4)[0] != 0x10:
        raise SystemExit("not a package manifest container: " + path)
    raw_len, comp_len = struct.unpack_from("<QQ", raw, 8)
    body = zstd_decompress(raw[24:24 + comp_len])
    if len(body) != raw_len:
        raise SystemExit("manifest length mismatch: %d != %d" % (len(body), raw_len))
    n_res, n_files, n_chunks = (struct.unpack_from("<Q", body, off)[0] for off in (0x38, 0x78, 0xb8))
    base_files = 0xc0 + 0x20 * n_res
    base_chunks = base_files + 0x28 * n_files
    if base_chunks + 0x10 * n_chunks != len(body):
        raise SystemExit("manifest arrays do not account for its length")
    resources = [struct.unpack_from("<QQIIII", body, 0xc0 + 0x20 * i) for i in range(n_res)]
    chunks = [struct.unpack_from("<IIII", body, base_chunks + 0x10 * i) for i in range(n_chunks)]
    return resources, chunks


def symbol(value, hasher):
    return int(value, 16) if value.lower().startswith("0x") else hasher(value)


def find_resource(data_dir, manifest_id, want_type, want_name):
    """Return the resource's bytes, read from the package chunk the manifest names."""
    resources, chunks = read_manifest(os.path.join(data_dir, "manifests", manifest_id))
    hits = [r for r in resources if r[0] == want_type and r[1] == want_name]
    if len(hits) != 1:
        raise SystemExit("expected exactly one resource 0x%016x:0x%016x, found %d"
                         % (want_type, want_name, len(hits)))
    _, _, chunk_idx, offset, size, _ = hits[0]
    package, file_off, comp, uncomp = chunks[chunk_idx]
    pkg_path = os.path.join(data_dir, "packages", "%s_%d" % (manifest_id, package))
    with open(pkg_path, "rb") as f:
        f.seek(file_off)
        block = zstd_decompress(f.read(comp))
    if len(block) != uncomp:
        raise SystemExit("chunk %d decompressed to %d bytes, manifest says %d" % (chunk_idx, len(block), uncomp))
    payload = block[offset:offset + size]
    if len(payload) != size:
        raise SystemExit("resource runs past its chunk")
    sys.stderr.write("type=0x%016x name=0x%016x chunk=%d package=%d bytes=%d\n"
                     % (want_type, want_name, chunk_idx, package, size))
    return payload


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--data", required=True)
    ap.add_argument("--manifest-id", required=True, help="16 hex digits, the file name under manifests/")
    ap.add_argument("--type", required=True)
    ap.add_argument("--name", required=True)
    ap.add_argument("--out")
    ap.add_argument("--c-array", metavar="IDENT")
    args = ap.parse_args()
    if not args.out and not args.c_array:
        ap.error("give --out or --c-array")

    repo_root = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..", ".."))
    hasher = load_hash(repo_root)
    want_type = symbol(args.type, hasher)
    want_name = symbol(args.name, hasher)

    payload = find_resource(args.data, args.manifest_id, want_type, want_name)
    size = len(payload)

    if args.out:
        open(args.out, "wb").write(payload)
    if args.c_array:
        print("static const unsigned char %s[%d] = {" % (args.c_array, size))
        for i in range(0, size, 16):
            print("    " + ", ".join("0x%02x" % b for b in payload[i:i + 16]) + ",")
        print("};")


if __name__ == "__main__":
    main()
