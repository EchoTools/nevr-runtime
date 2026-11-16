#!/usr/bin/env python3
"""Make the PLUGINS-card override set (#1857) from the player's own game install.

The set is 13 resource override files for the main-menu level (`mnu_master`, name hash 0xac360e41e4ede056), each named
`0x<resource type>.0x<level name>` as `_overrides/` expects. Each one is the stock resource of that type with the edits
that add a fifth card. The repository holds no byte of the game's own data (RELEASING.md "Game-derived packages"):
`plugins_override_patches.json` holds offsets, lengths, hashes and bytes of our own, and nothing else. Per file:

  * the SHA-256 and size of the stock resource it must be built from (a different game version is refused), and
  * the edit list, in four kinds:
      ["c", offset, length]          copy a range of this file's stock resource
      ["x", type, offset, length]    copy a range of another file's stock resource (`type` is that file's override type)
      ["h", name]                    the 8 bytes of the game symbol hash of `name` (computed here; the table holds the name)
      ["p", hex]                     bytes of our own

The rule for a put: it carries no stock run. Every put of 8 bytes or more is one of the values the table lists by name in
`ours`, each with its reason (the new actor's hash, the card's component id, the card's pose); every other put is shorter
than 8 bytes (counts, sizes, header words); and no 8-byte window of any put occurs in any stock resource of the level
(checked against the player's install on every `generate` and `derive`, `PutCarriesStock`). A stock run, a symbol hash
included, is a copy or a `h`, never a put.

Usage:
    plugins_override_generator.py generate --data-dir <game _data/<id> folder> --out <empty folder>
    plugins_override_generator.py derive   --data-dir <game _data/<id> folder> --reference <override set> --out <patches.json>

`--data-dir` may also be given as the environment variable NEVR_GAME_DATA_DIR. There is no default. It is read, never
written: an `--out` inside it is refused. `derive` is for the maintainer who changes the card: it rebuilds the edit list
from a finished override set and the maintainer's own install, and refuses to write a put that carries a stock run or is
not listed.

Needs `pip install zstandard` (through rad_archive_tool) for the two commands that read the install.
"""
from __future__ import annotations

import argparse
import collections
import hashlib
import json
import os
import struct
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
from radhash import rad_hash  # noqa: E402

PATCHES = HERE / "plugins_override_patches.json"
DATA_DIR_ENV = "NEVR_GAME_DATA_DIR"
# The window of a put that may not occur in stock, and the shortest run `derive` writes as a copy. A put shorter than this
# has no window, so it is a count, a size or a header word.
WINDOW = 8
# Distinct byte values a window needs before it counts as data (see is_data).
DISTINCT_DATA = 3
EXIT_REFUSED = 2


class OverrideError(Exception):
    """A refusal with a name: the message starts with the class name, so a log line says which check stopped the run."""

    def __str__(self) -> str:
        return f"{type(self).__name__}: {super().__str__()}"


class UsageError(OverrideError):
    """No game data directory, or an output folder that is not allowed."""


class StockMissing(OverrideError):
    """The install has no resource of the type the edit list is built on, or has more than one."""


class StockMismatch(OverrideError):
    """The stock resource is not the one the edit list was made from: another game version, or a changed install."""


class PatchTableError(OverrideError):
    """The edit list is malformed or does not fit the stock resource it names."""


class PutNotOurs(PatchTableError):
    """A put of 8 bytes or more that the table does not list by name in `ours`."""


class PutCarriesStock(OverrideError):
    """A put holds a run of 8 bytes that occurs in a stock resource of the level: it must be a copy or a symbol hash."""


class OutputMismatch(OverrideError):
    """The edits applied cleanly but the result is not the recorded file: the edit list and its recorded hash disagree."""


def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def is_data(window: bytes) -> bool:
    """A window of 3 or more distinct byte values can carry data. One of 1 or 2 (zero padding, a count, a sentinel) is
    structure: it is the same in any resource, so finding it in stock proves nothing."""
    return len(set(window)) >= DISTINCT_DATA


def symbol_bytes(name: str) -> bytes:
    return struct.pack("<Q", rad_hash(name))


def file_name(entry: dict, level: str) -> str:
    return f"{entry['override_type']}.{level}"


def ours_values(table: dict) -> dict[str, dict]:
    return {o["hex"]: o for o in table.get("ours", [])}


def check_table(table: dict) -> None:
    """The rules a table must keep without the game: every put of 8 bytes or more is listed, every reference resolves."""
    listed = {}
    for o in table.get("ours", []):
        if not all(isinstance(o.get(k), str) and o[k] for k in ("name", "hex", "reason")):
            raise PatchTableError(f"an `ours` entry needs a name, a hex value and a reason: {o!r}")
        try:
            raw = bytes.fromhex(o["hex"])
        except ValueError as exc:
            raise PatchTableError(f"`ours` entry {o['name']}: not hex ({exc})") from exc
        if len(raw) < WINDOW:
            raise PatchTableError(f"`ours` entry {o['name']} is {len(raw)} bytes: only values of {WINDOW} bytes or more are listed")
        if o["hex"] in listed:
            raise PatchTableError(f"`ours` lists {o['hex']} twice")
        listed[o["hex"]] = o["name"]
    sizes = {e["override_type"]: e["stock_size"] for e in table["files"]}
    for e in table["files"]:
        for n, op in enumerate(e["ops"]):
            where = f"{e['override_type']} edit {n}"
            if op[0] == "p" and len(op) == 2:
                raw = bytes.fromhex(op[1])
                if len(raw) >= WINDOW and is_data(raw) and op[1] not in listed:
                    raise PutNotOurs(f"{where}: a put of {len(raw)} bytes ({op[1]}) is not listed in `ours`; a stock run is a copy, not a put")
            elif op[0] == "x" and len(op) == 4:
                if op[1] not in sizes:
                    raise PatchTableError(f"{where}: copy from {op[1]}, which is not a file of the set")
                if not (0 <= op[2] and 0 < op[3] and op[2] + op[3] <= sizes[op[1]]):
                    raise PatchTableError(f"{where}: copy [{op[2]}, +{op[3]}] lies outside the {sizes[op[1]]}-byte stock resource {op[1]}")
            elif op[0] == "h" and len(op) == 2:
                if not (isinstance(op[1], str) and op[1]):
                    raise PatchTableError(f"{where}: a symbol hash needs a name")
            elif op[0] == "c" and len(op) == 3:
                if not (0 <= op[1] and 0 < op[2] and op[1] + op[2] <= e["stock_size"]):
                    raise PatchTableError(f"{where}: copy [{op[1]}, +{op[2]}] lies outside the {e['stock_size']}-byte stock resource")
            else:
                raise PatchTableError(f"{where}: not a copy, a cross copy, a symbol hash or a put: {op!r}")


def load_table(path: Path = PATCHES) -> dict:
    table = json.loads(path.read_text())
    if table.get("schema") != 2:
        raise PatchTableError(f"{path} has schema {table.get('schema')!r}, this tool reads schema 2")
    check_table(table)
    return table


def apply_ops(stock: bytes, ops: list, name: str = "?", others: dict[str, bytes] | None = None) -> bytes:
    """Build a file from the stock resource and an edit list. A copy outside the stock is an error, never a clamp."""
    out = bytearray()
    for n, op in enumerate(ops):
        where = f"{name} edit {n}"
        if op[0] in ("c", "x") and len(op) == (3 if op[0] == "c" else 4):
            source = stock
            if op[0] == "x":
                if op[1] not in (others or {}):
                    raise PatchTableError(f"{where}: copy from {op[1]}, whose stock resource was not given")
                source = others[op[1]]
            off, length = op[-2], op[-1]
            if not (isinstance(off, int) and isinstance(length, int) and 0 <= off and 0 < length and off + length <= len(source)):
                raise PatchTableError(f"{where}: copy [{off}, +{length}] lies outside the {len(source)}-byte stock resource")
            out += source[off:off + length]
        elif op[0] == "h" and len(op) == 2 and isinstance(op[1], str) and op[1]:
            out += symbol_bytes(op[1])
        elif op[0] == "p" and len(op) == 2:
            try:
                out += bytes.fromhex(op[1])
            except (TypeError, ValueError) as exc:
                raise PatchTableError(f"{where}: put is not hex ({exc})") from exc
        else:
            raise PatchTableError(f"{where}: not a copy, a cross copy, a symbol hash or a put: {op!r}")
    return bytes(out)


def check_puts_clean(ops: list, resources: list[bytes], name: str = "?") -> None:
    """No data window (8 bytes, 3 or more distinct values) of any put occurs in any stock resource of the level."""
    for n, op in enumerate(ops):
        if op[0] != "p":
            continue
        raw = bytes.fromhex(op[1])
        for i in range(len(raw) - WINDOW + 1):
            window = raw[i:i + WINDOW]
            if is_data(window) and any(window in res for res in resources):
                raise PutCarriesStock(f"{name} edit {n}: the put {op[1]} carries the stock run {window.hex()} at byte {i}")


def build_file(entry: dict, stock: bytes, others: dict[str, bytes] | None = None, resources: list[bytes] | None = None) -> bytes:
    """One override file from its table entry and the stock resources: stock checked first, result checked last."""
    name = entry["override_type"]
    if sha256(stock) != entry["stock_sha256"]:  # the size is in the hash; it is recorded for the reader
        raise StockMismatch(
            f"{name} ({entry['stock_class']}): the install's resource is {len(stock)} bytes, sha256 {sha256(stock)}; "
            f"the edit list was made from {entry['stock_size']} bytes, sha256 {entry['stock_sha256']} (another game version?)")
    if resources is not None:
        check_puts_clean(entry["ops"], resources, name)
    result = apply_ops(stock, entry["ops"], name, others)
    if len(result) != entry["size"] or sha256(result) != entry["sha256"]:
        raise OutputMismatch(f"{name}: the edits give {len(result)} bytes, sha256 {sha256(result)}; recorded {entry['size']} bytes, sha256 {entry['sha256']}")
    return result


def read_level(data_dir: Path, level_hash: int) -> dict[int, list[bytes]]:
    """Every resource of the level in the archives under `data_dir`, by type (through rad_archive_tool)."""
    sys.path.insert(0, str(HERE))
    import rad_archive_tool as rad  # imported here: it exits the process when zstandard is missing

    archives = rad.find_archives(data_dir)
    if not archives:
        raise StockMissing(f"no archive (manifests and packages folders) under {data_dir}")
    found: dict[int, list[bytes]] = collections.defaultdict(list)
    for paths in archives.values():
        manifest = rad.Manifest.parse(rad.read_mf(str(paths["mf"])))
        packages = {int(p.name.rsplit("_", 1)[1]): p for p in paths["pkgs"]}
        blocks: dict[int, bytes] = {}
        for res in manifest.res:
            if res.nh != level_hash:
                continue
            if res.bi not in blocks:
                blk = manifest.blks[res.bi]
                with open(packages[blk.pi], "rb") as fh:
                    fh.seek(blk.off)
                    compressed = fh.read(blk.csz)
                blocks[res.bi] = rad.zstandard.ZstdDecompressor().decompress(compressed, max_output_size=blk.dsz + 65536)
            found[res.th].append(blocks[res.bi][res.off:res.off + res.rsz])
    return dict(found)


def pick_stock(level: dict[int, list[bytes]], level_hash: int, type_hashes: list[int]) -> dict[int, bytes]:
    """The one stock resource of each wanted type."""
    out = {}
    for th in type_hashes:
        have = level.get(th, [])
        if len(have) != 1:
            raise StockMissing(f"type 0x{th:016x} of level 0x{level_hash:016x}: {len(have)} resources, expected exactly 1")
        out[th] = have[0]
    return out


def data_dir_from(arg: str | None) -> Path:
    value = arg or os.environ.get(DATA_DIR_ENV)
    if not value:
        raise UsageError(f"no game data directory: pass --data-dir <the game's _data/<id> folder> or set {DATA_DIR_ENV}")
    path = Path(value)
    if not path.is_dir():
        raise UsageError(f"--data-dir / {DATA_DIR_ENV} {value} is not a directory")
    return path


def load_level(table: dict, data_dir: Path) -> tuple[dict[str, bytes], list[bytes]]:
    """The stock resource of each file of the table (by override type), and every resource of the level."""
    level_hash = int(table["level"], 16)
    level = read_level(data_dir, level_hash)
    by_type = pick_stock(level, level_hash, [int(e["stock_type"], 16) for e in table["files"]])
    stocks = {e["override_type"]: by_type[int(e["stock_type"], 16)] for e in table["files"]}
    return stocks, [res for many in level.values() for res in many]


def generate(table: dict, stocks: dict[str, bytes], out_dir: Path, data_dir: Path | None = None, force: bool = False,
             resources: list[bytes] | None = None) -> list[Path]:
    """Write every file of the set into `out_dir`. Nothing is written unless every file builds."""
    out_dir = out_dir.resolve()
    if data_dir is not None and (out_dir == data_dir.resolve() or data_dir.resolve() in out_dir.parents):
        raise UsageError(f"--out {out_dir} is inside the game data directory {data_dir}: the install is never written")
    built = {}
    for entry in table["files"]:
        if entry["override_type"] not in stocks:
            raise StockMissing(f"no stock resource given for {entry['override_type']}")
        built[file_name(entry, table["level"])] = build_file(entry, stocks[entry["override_type"]], stocks, resources)
    existing = [n for n in built if (out_dir / n).exists()]
    if existing and not force:
        raise UsageError(f"{out_dir} already has {existing[0]} (and {len(existing) - 1} more): pass --force to replace them")
    out_dir.mkdir(parents=True, exist_ok=True)
    paths = []
    for name, data in built.items():
        (out_dir / name).write_bytes(data)
        paths.append(out_dir / name)
    return paths


def derive_ops(stock: bytes, target: bytes, others: dict[str, bytes] | None = None, ours: list[bytes] | None = None,
               symbols: list[str] | None = None) -> list:
    """An edit list that turns `stock` into `target`.

    The values of `ours` and the hashes of `symbols` are found in the target first and kept whole. Everything else is a copy
    of a run of 8 bytes or more of this file's stock (nearest the last copy when several fit) or of another file's stock,
    and what no stock supplies is a put. A put that replaces an equal number of stock bytes is cut to the bytes that differ.
    """
    others = others or {}
    sources = [(None, stock)] + sorted(others.items())
    index: dict[bytes, list] = collections.defaultdict(list)
    for key, data in sources:
        for i in range(len(data) - WINDOW + 1):
            if key is None or is_data(data[i:i + WINDOW]):  # padding, counts and sentinels match anywhere: copied from this file only
                index[data[i:i + WINDOW]].append((key, i))
    pins: dict[int, tuple[int, list]] = {}
    pinned = [(v, ["p", v.hex()]) for v in (ours or [])] + [(symbol_bytes(s), ["h", s]) for s in (symbols or [])]
    for value, op in pinned:
        at = target.find(value)
        while at != -1:
            if not any(s < at + len(value) and at < s + n for s, (n, _) in pins.items()):
                pins[at] = (len(value), op)
            at = target.find(value, at + 1)
    starts = sorted(pins)
    ops: list = []
    pending = bytearray()

    def flush() -> None:
        if pending:
            ops.append(["p", bytes(pending).hex()])
            pending.clear()

    expect = 0  # where the previous copy of this file's stock ended: of equal runs, the nearest to it is taken
    j = 0
    while j < len(target):
        if j in pins:
            flush()
            length, op = pins[j]
            ops.append(list(op))
            j += length
            continue
        cap = min([s for s in starts if s > j] + [len(target)]) - j
        best = None
        for key, c in index.get(target[j:j + WINDOW], []):
            data = stock if key is None else others[key]
            n = 0
            while c + n < len(data) and n < cap and target[j + n] == data[c + n]:
                n += 1
            rank = (key is None, n, -abs(c - expect) if key is None else 0)  # this file's own stock first
            if n >= WINDOW and (best is None or rank > best[0]):
                best = (rank, key, c, n)
        if best is not None:
            flush()
            _, key, c, n = best
            ops.append(["c", c, n] if key is None else ["x", key, c, n])
            if key is None:
                expect = c + n
            j += n
        else:
            pending.append(target[j])
            j += 1
    flush()
    return merge_copies(cut_in_place_puts(ops, stock, {v.hex() for v in (ours or [])}))


def cut_in_place_puts(ops: list, stock: bytes, keep: set[str]) -> list:
    """A put between two copies of this file's stock (or at either end of the file) that replaces as many stock bytes as it
    holds keeps only the bytes that differ from stock; the equal bytes become copies."""
    out: list = []
    for n, op in enumerate(ops):
        prev, nxt = ops[n - 1] if n else None, ops[n + 1] if n + 1 < len(ops) else None
        starts_at = 0 if prev is None else (prev[1] + prev[2] if prev[0] == "c" else None)
        ends_at = len(stock) if nxt is None else (nxt[1] if nxt[0] == "c" else None)
        if op[0] == "p" and op[1] not in keep and starts_at is not None and ends_at is not None:
            raw = bytes.fromhex(op[1])
            start = starts_at
            if ends_at - start == len(raw):
                i = 0
                while i < len(raw):
                    j = i
                    same = raw[i] == stock[start + i]
                    while j < len(raw) and (raw[j] == stock[start + j]) == same:
                        j += 1
                    out.append(["c", start + i, j - i] if same else ["p", raw[i:j].hex()])
                    i = j
                continue
        out.append(op)
    return out


def merge_copies(ops: list) -> list:
    merged: list = []
    for op in ops:
        if merged and op[0] == "c" and merged[-1][0] == "c" and merged[-1][1] + merged[-1][2] == op[1]:
            merged[-1] = ["c", merged[-1][1], merged[-1][2] + op[2]]
        else:
            merged.append(op)
    return merged


def derive(table: dict, stocks: dict[str, bytes], reference: Path, resources: list[bytes] | None = None) -> dict:
    """The table with each file's edit list rebuilt from the finished set in `reference`, refusing a put that is not ours."""
    ours = [bytes.fromhex(o["hex"]) for o in table.get("ours", [])]
    files = []
    for entry in table["files"]:
        target = (reference / file_name(entry, table["level"])).read_bytes()
        stock = stocks[entry["override_type"]]
        others = {k: v for k, v in stocks.items() if k != entry["override_type"]}
        ops = derive_ops(stock, target, others, ours, table.get("symbols", []))
        if apply_ops(stock, ops, entry["override_type"], others) != target:
            raise OutputMismatch(f"{entry['override_type']}: the derived edits do not rebuild the reference file")
        if resources is not None:
            check_puts_clean(ops, resources, entry["override_type"])
        files.append({**entry, "stock_size": len(stock), "stock_sha256": sha256(stock), "size": len(target), "sha256": sha256(target), "ops": ops})
    result = {**table, "files": files}
    check_table(result)
    return result


def dump_table(table: dict) -> str:
    """One line per edit so a changed card is a readable diff."""
    lines = ["{", f'  "schema": {table["schema"]},', f'  "level": {json.dumps(table["level"])},',
             f'  "symbols": {json.dumps(table.get("symbols", []))},', '  "ours": [']
    lines.append(",\n".join("    " + json.dumps(o) for o in table.get("ours", [])))
    lines += ["  ],", '  "files": [']
    for n, entry in enumerate(table["files"]):
        head = {k: v for k, v in entry.items() if k != "ops"}
        lines.append("    {" + ", ".join(f"{json.dumps(k)}: {json.dumps(v)}" for k, v in head.items()) + ',')
        lines.append('     "ops": [')
        lines.append(",\n".join("       " + json.dumps(op) for op in entry["ops"]))
        lines.append("     ]}" + ("," if n + 1 < len(table["files"]) else ""))
    lines += ["  ]", "}"]
    return "\n".join(lines) + "\n"


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest="command", required=True)
    gen = sub.add_parser("generate", help="write the 13 override files")
    gen.add_argument("--data-dir", help=f"the game's _data/<id> folder (or set {DATA_DIR_ENV})")
    gen.add_argument("--out", required=True, type=Path, help="folder to write the files into (its `_overrides` copy, not the install)")
    gen.add_argument("--force", action="store_true", help="replace files already in --out")
    der = sub.add_parser("derive", help="rebuild the edit list from an override set made by other means")
    der.add_argument("--data-dir", help=f"the game's _data/<id> folder (or set {DATA_DIR_ENV})")
    der.add_argument("--reference", required=True, type=Path, help="folder holding the 13 finished files")
    der.add_argument("--out", required=True, type=Path, help="the patches file to write")
    for command in (gen, der):
        command.add_argument("--patches", type=Path, default=PATCHES, help="the edit list (default: plugins_override_patches.json beside this tool)")
    args = parser.parse_args(argv)
    try:
        data_dir = data_dir_from(args.data_dir)
        if args.command == "generate":
            table = load_table(args.patches)
            stocks, resources = load_level(table, data_dir)
            paths = generate(table, stocks, args.out, data_dir, args.force, resources)
            for path in paths:
                print(f"wrote {path.name} {path.stat().st_size} bytes sha256 {sha256(path.read_bytes())}")
            print(f"plugins_override_generator: wrote {len(paths)} files to {args.out}")
        else:
            table = json.loads(args.patches.read_text())
            stocks, resources = load_level(table, data_dir)
            args.out.write_text(dump_table(derive(table, stocks, args.reference, resources)))
            print(f"plugins_override_generator: wrote {args.out}")
    except OverrideError as exc:
        print(f"plugins_override_generator: refused: {exc}", file=sys.stderr)
        return EXIT_REFUSED
    return 0


if __name__ == "__main__":
    sys.exit(main())
