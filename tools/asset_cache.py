#!/usr/bin/env python3
"""Put a file into the asset cache, or verify a cache. The layout is src/runtime/patch/asset_cache.h's:

  <sha256>.blob   the bytes, named by their SHA-256
  index.json      {"version":1,"overrides":[{"type":"0x..","name":"0x..","sha256":"..","label":".."}]}

  asset_cache.py put    --dir DIR --type 0xTYPE --name 0xNAME [--label L] FILE
  asset_cache.py verify --dir DIR

The type names the buffer the blob replaces (src/runtime/patch/resource_buffer_key.h): a texture's payload is
`--type 0xbeac1969cb7b8861` (Make(CGTextureResource, "Win10GPU"), buffer 1); its logical type or 0 names buffer 0.

DIR is `%LOCALAPPDATA%\\nevr\\echovr\\cache` of the Windows user (under Wine: `<prefix>/drive_c/users/<user>/AppData/Local/nevr/echovr/cache`).
A blob is written to a temp file, flushed and renamed into place; index.json the same way. A damaged index.json is
never overwritten. Exit 0 and `ok:` lines on success; exit 1 and a `FAIL:` line on stderr otherwise. Standard library only.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import sys
import time

BLOB_SUFFIX = ".blob"
INDEX_NAME = "index.json"
MAX_BLOB_BYTES = 128 * 1024 * 1024
SHA = re.compile(r"^[0-9a-f]{64}$")
HASH = re.compile(r"^0[xX][0-9a-fA-F]{1,16}$")


class Failure(Exception):
    pass


def sha256_hex(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def blob_path(directory: str, sha: str) -> str:
    return os.path.join(directory, sha + BLOB_SUFFIX)


def write_atomically(directory: str, final_name: str, stem: str, data: bytes) -> None:
    temp = os.path.join(directory, f"{stem}.{os.getpid()}.{time.monotonic_ns()}.tmp")
    try:
        with open(temp, "wb") as handle:
            handle.write(data)
            handle.flush()
            os.fsync(handle.fileno())
        os.replace(temp, os.path.join(directory, final_name))
    except OSError:
        try:
            os.unlink(temp)
        except OSError:
            pass
        raise


def read_index(directory: str) -> list[dict]:
    path = os.path.join(directory, INDEX_NAME)
    if not os.path.exists(path):
        return []
    try:
        with open(path, encoding="utf-8") as handle:
            doc = json.load(handle)
    except (OSError, ValueError) as error:
        raise Failure(f"{path} is unreadable ({error}); not overwriting it")
    if not isinstance(doc, dict) or doc.get("version") != 1 or not isinstance(doc.get("overrides"), list):
        raise Failure(f"{path} is not a version 1 index; not overwriting it")
    return [e for e in doc["overrides"] if isinstance(e, dict)]


def hash_value(text: str) -> int:
    if not HASH.match(text):
        raise Failure(f"{text!r} is not a 0x hash (1 to 16 hex digits)")
    return int(text, 16)


def put(directory: str, type_text: str, name_text: str, label: str, source: str) -> str:
    type_hash, name_hash = hash_value(type_text), hash_value(name_text)
    if name_hash == 0:
        raise Failure("the name hash must not be 0")
    with open(source, "rb") as handle:
        data = handle.read(MAX_BLOB_BYTES + 1)
    if len(data) > MAX_BLOB_BYTES:
        raise Failure(f"{source} is larger than {MAX_BLOB_BYTES} bytes")
    entries = read_index(directory)  # before anything is written: a damaged index stops the put
    os.makedirs(directory, exist_ok=True)
    sha = sha256_hex(data)
    existing = blob_path(directory, sha)
    if not (os.path.exists(existing) and sha256_hex(open(existing, "rb").read()) == sha):
        write_atomically(directory, sha + BLOB_SUFFIX, sha, data)
    key = (f"0x{type_hash:016x}", f"0x{name_hash:016x}")
    entry = {"type": key[0], "name": key[1], "sha256": sha, "label": label}
    kept = []
    replaced = False
    for e in entries:
        same = (str(e.get("type", "")).lower(), str(e.get("name", "")).lower()) == key
        if same and not replaced:
            kept.append(entry)
            replaced = True
        elif not same:
            kept.append(e)
    if not replaced:
        kept.append(entry)
    text = json.dumps({"version": 1, "overrides": kept}, indent=2) + "\n"
    write_atomically(directory, INDEX_NAME, "index", text.encode("utf-8"))
    return sha


def verify(directory: str) -> list[str]:
    problems = []
    entries = read_index(directory)
    for e in entries:
        sha = e.get("sha256", "")
        if not isinstance(sha, str) or not SHA.match(sha):
            problems.append(f"index entry {e!r}: sha256 is not 64 lowercase hex")
            continue
        path = blob_path(directory, sha)
        if not os.path.exists(path):
            problems.append(f"{sha}: blob missing")
        elif sha256_hex(open(path, "rb").read()) != sha:
            problems.append(f"{sha}: blob does not hash to its name")
    for name in sorted(os.listdir(directory)) if os.path.isdir(directory) else []:
        if name.endswith(BLOB_SUFFIX) and SHA.match(name[: -len(BLOB_SUFFIX)]):
            if sha256_hex(open(os.path.join(directory, name), "rb").read()) != name[: -len(BLOB_SUFFIX)]:
                problems.append(f"{name}: blob does not hash to its name")
    return sorted(set(problems))


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = parser.add_subparsers(dest="command", required=True)
    p_put = sub.add_parser("put")
    p_put.add_argument("--dir", required=True)
    p_put.add_argument("--type", required=True)
    p_put.add_argument("--name", required=True)
    p_put.add_argument("--label", default="")
    p_put.add_argument("file")
    p_verify = sub.add_parser("verify")
    p_verify.add_argument("--dir", required=True)
    args = parser.parse_args(argv)
    try:
        if args.command == "put":
            sha = put(args.dir, args.type, args.name, args.label, args.file)
            print(f"ok: stored sha256={sha} path={blob_path(args.dir, sha)}")
            return 0
        problems = verify(args.dir)
        if problems:
            for problem in problems:
                print(f"FAIL: {problem}", file=sys.stderr)
            return 1
        print(f"ok: {args.dir} verifies")
        return 0
    except (Failure, OSError) as error:
        print(f"FAIL: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
