#!/usr/bin/env python3
"""Put a file into the asset cache, or verify a cache. The layout is src/runtime/patch/asset_cache.h's:

  <sha256>.blob   the bytes, named by their SHA-256
  index.json      {"version":1,"overrides":[{"type":"0x..","name":"0x..","sha256":"..","label":".."}]}
  owned.txt       one name per line: every blob and temp this tool or the runtime made (asset_cache_index.h)

  asset_cache.py put    --dir DIR --type 0xTYPE --name 0xNAME [--label L] FILE
  asset_cache.py verify --dir DIR

The type names the buffer the blob replaces (src/runtime/patch/resource_buffer_key.h): a texture's payload is
`--type 0xbeac1969cb7b8861` (Make(CGTextureResource, "Win10GPU"), buffer 1); `0x4a4c32c49300b8a0` (Make(CGTextureResource,
"Win10")), its logical type or 0 name buffer 0, the descriptor. A payload of another size than the game's needs its
descriptor entry too.

DIR is `%LOCALAPPDATA%\\nevr\\echovr\\cache` of the Windows user (under Wine: `<prefix>/drive_c/users/<user>/AppData/Local/nevr/echovr/cache`).
Before a temp file is created its name is recorded in owned.txt, synced, and a blob the tool writes is listed there too, so the
runtime's Evict can remove it by name (ORDER.md "The manifest rule"). A blob or temp is written to a temp file, flushed and renamed
into place; index.json the same way. A damaged index.json is never overwritten; a damaged owned.txt has its bad lines dropped and is rewritten clean. Exit 0 and `ok:` lines on success;
exit 1 and a `FAIL:` line on stderr otherwise. Standard library only.
"""
from __future__ import annotations

import argparse
import hashlib
import itertools
import json
import os
import re
import sys
import time

BLOB_SUFFIX = ".blob"
INDEX_NAME = "index.json"
OWNED_NAME = "owned.txt"
OWNED_TEMP_NAME = "owned.txt.tmp"
MAX_BLOB_BYTES = 128 * 1024 * 1024
MAX_OWNED_BYTES = 4 * 1024 * 1024
SHA = re.compile(r"^[0-9a-f]{64}$")
HASH = re.compile(r"^0[xX][0-9a-fA-F]{1,16}$")
# The grammar of asset_cache_index.h (IsBlobLeaf, IsTempLeaf): fullmatch, ASCII, no separators.
OWNED_BLOB = re.compile(r"[0-9a-f]{64}\.blob")
OWNED_TEMP = re.compile(r"(?:[0-9a-f]{64}|index)\.[0-9]{1,20}\.[0-9]{1,20}\.[0-9]{1,20}\.tmp")
_SEQUENCE = itertools.count()


class Failure(Exception):
    pass


def sha256_hex(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def is_owned_leaf(name: str) -> bool:
    return OWNED_BLOB.fullmatch(name) is not None or OWNED_TEMP.fullmatch(name) is not None


def blob_path(directory: str, sha: str) -> str:
    return os.path.join(directory, sha + BLOB_SUFFIX)


def temp_leaf(stem: str) -> str:
    """`<stem>.<pid>.<tick>.<sequence>.tmp`, the C++ TempPathFor shape: a fresh name for each write."""
    return f"{stem}.{os.getpid()}.{time.monotonic_ns()}.{next(_SEQUENCE)}.tmp"


def write_file_synced(directory: str, temp: str, final: str, data: bytes) -> None:
    """Writes `data` to `temp` (a name already recorded in owned.txt), syncs it, renames it over `final`."""
    temp_path = os.path.join(directory, temp)
    try:
        with open(temp_path, "wb") as handle:
            handle.write(data)
            handle.flush()
            os.fsync(handle.fileno())
        os.replace(temp_path, os.path.join(directory, final))
    except OSError:
        try:
            os.unlink(temp_path)
        except OSError:
            pass
        raise


def parse_owned(raw: bytes) -> tuple[list[str], int]:
    """owned.txt's names and the number of lines dropped, with asset_cache_index.h's rules: a line that is not a name of this cache
    (a NUL, a traversal, anything else) is dropped and counted, never trusted; a CR before the LF is tolerated; a last line with no
    newline is a torn append and is dropped. The list is refused only when it is over the size cap."""
    if len(raw) > MAX_OWNED_BYTES:
        raise Failure("owned.txt is larger than the cache reads; not overwriting it")
    text = raw.decode("ascii", errors="replace")  # a byte that is not ASCII becomes U+FFFD and fails the grammar
    names = []
    lost = 0
    start = 0
    while True:
        newline = text.find("\n", start)
        if newline < 0:
            if start < len(text):
                lost += 1  # torn append
            break
        line = text[start:newline]
        if line.endswith("\r"):
            line = line[:-1]
        if line:
            if is_owned_leaf(line):
                names.append(line)
            else:
                lost += 1
        start = newline + 1
    return names, lost


def write_owned(directory: str, names: list[str]) -> None:
    text = "".join(name + "\n" for name in names).encode("ascii")
    write_file_synced(directory, OWNED_TEMP_NAME, OWNED_NAME, text)


def ensure_owned(directory: str) -> list[str]:
    """The names owned.txt lists. Without the file (a cache from before it) this adopts, once, each blob whose content hashes to
    its own name, as asset_cache.cpp's EnsureOwnedIndex does; a temp is never adopted, and nothing else in the folder is listed."""
    path = os.path.join(directory, OWNED_NAME)
    if os.path.exists(path):
        with open(path, "rb") as handle:
            return parse_owned(handle.read(MAX_OWNED_BYTES + 1))[0]
    names = []
    for name in sorted(os.listdir(directory)):
        if OWNED_BLOB.fullmatch(name) is None or os.path.isdir(os.path.join(directory, name)):
            continue
        with open(os.path.join(directory, name), "rb") as handle:
            data = handle.read(MAX_BLOB_BYTES + 1)
        if len(data) <= MAX_BLOB_BYTES and sha256_hex(data) == name[:64]:
            names.append(name)
    write_owned(directory, names)
    return names


def record_owned(directory: str, leaves: list[str]) -> None:
    """Lists each name not already listed, and makes the list durable before the caller creates any of those files. A name the
    list already holds is not listed twice."""
    names = ensure_owned(directory)
    added = []
    for leaf in leaves:
        if leaf not in names and leaf not in added:
            added.append(leaf)
    if added:
        write_owned(directory, names + added)


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
        blob_temp = temp_leaf(sha)
        record_owned(directory, [blob_temp, sha + BLOB_SUFFIX])  # both names listed before either file is made
        write_file_synced(directory, blob_temp, sha + BLOB_SUFFIX, data)
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
    index_temp = temp_leaf("index")
    record_owned(directory, [index_temp])
    write_file_synced(directory, index_temp, INDEX_NAME, text.encode("utf-8"))
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
