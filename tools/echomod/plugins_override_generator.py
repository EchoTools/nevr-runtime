#!/usr/bin/env python3
"""Make the PLUGINS-card override set (#1857) from the player's own game install.

The set is 13 resource override files for the main-menu level (`mnu_master`, name hash 0xac360e41e4ede056), each named
`0x<resource type>.0x<level name>` as `_overrides/` expects. Each one is the stock resource of that type with the edits
that add a fifth card. No byte of the game's own data is stored in the repository (RELEASING.md "Game-derived packages"):
`plugins_override_patches.json` holds, per file, only

  * the SHA-256 and size of the stock resource it must be built from (a different game version is refused), and
  * the edit list: ["c", offset, length] copies a range of the stock resource, ["p", hex] puts bytes of our own
    (the new actor's hash, counts, sizes, the card's text).

Usage:
    plugins_override_generator.py generate --data-dir <game _data/<id> folder> --out <empty folder>
    plugins_override_generator.py derive   --data-dir <game _data/<id> folder> --reference <override set> --out <patches.json>

`--data-dir` may also be given as the environment variable NEVR_GAME_DATA_DIR. There is no default. It is read, never
written: an `--out` inside it is refused. `derive` is for the maintainer who changes the card: it rebuilds the edit list
from an override set made some other way.

Needs `pip install zstandard` (through rad_archive_tool) for the two commands that read the install.
"""
from __future__ import annotations

import argparse
import collections
import hashlib
import json
import os
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
PATCHES = HERE / "plugins_override_patches.json"
DATA_DIR_ENV = "NEVR_GAME_DATA_DIR"
# Shortest run of stock bytes `derive` writes as a copy; shorter runs are put as bytes (they are our values: hashes, counts).
MIN_COPY = 8
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


class OutputMismatch(OverrideError):
    """The edits applied cleanly but the result is not the recorded file: the edit list and its recorded hash disagree."""


def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def file_name(entry: dict, level: str) -> str:
    return f"{entry['override_type']}.{level}"


def load_table(path: Path = PATCHES) -> dict:
    table = json.loads(path.read_text())
    if table.get("schema") != 1:
        raise PatchTableError(f"{path} has schema {table.get('schema')!r}, this tool reads schema 1")
    return table


def apply_ops(stock: bytes, ops: list, name: str = "?") -> bytes:
    """Build a file from the stock resource and an edit list. A copy outside the stock resource is an error, never a clamp."""
    out = bytearray()
    for n, op in enumerate(ops):
        if op[0] == "c" and len(op) == 3:
            _, off, length = op
            if not (isinstance(off, int) and isinstance(length, int) and 0 <= off and 0 < length and off + length <= len(stock)):
                raise PatchTableError(f"{name} edit {n}: copy [{off}, +{length}] lies outside the {len(stock)}-byte stock resource")
            out += stock[off:off + length]
        elif op[0] == "p" and len(op) == 2:
            try:
                out += bytes.fromhex(op[1])
            except (TypeError, ValueError) as exc:
                raise PatchTableError(f"{name} edit {n}: put is not hex ({exc})") from exc
        else:
            raise PatchTableError(f"{name} edit {n}: not a copy [\"c\", offset, length] or a put [\"p\", hex]: {op!r}")
    return bytes(out)


def build_file(entry: dict, stock: bytes) -> bytes:
    """One override file from its table entry and the stock resource: stock checked first, result checked last."""
    name = entry["override_type"]
    if sha256(stock) != entry["stock_sha256"]:  # the size is in the hash; it is recorded for the reader

        raise StockMismatch(
            f"{name} ({entry['stock_class']}): the install's resource is {len(stock)} bytes, sha256 {sha256(stock)}; "
            f"the edit list was made from {entry['stock_size']} bytes, sha256 {entry['stock_sha256']} (another game version?)")
    result = apply_ops(stock, entry["ops"], name)
    if len(result) != entry["size"] or sha256(result) != entry["sha256"]:
        raise OutputMismatch(f"{name}: the edits give {len(result)} bytes, sha256 {sha256(result)}; recorded {entry['size']} bytes, sha256 {entry['sha256']}")
    return result


def read_stock(data_dir: Path, level_hash: int, type_hashes: list[int]) -> dict[int, bytes]:
    """The stock resource of each wanted type, for the level, from the archives under `data_dir` (through rad_archive_tool)."""
    sys.path.insert(0, str(HERE))
    import rad_archive_tool as rad  # imported here: it exits the process when zstandard is missing

    archives = rad.find_archives(data_dir)
    if not archives:
        raise StockMissing(f"no archive (manifests and packages folders) under {data_dir}")
    found: dict[int, list[bytes]] = collections.defaultdict(list)
    wanted = set(type_hashes)
    for name, paths in archives.items():
        manifest = rad.Manifest.parse(rad.read_mf(str(paths["mf"])))
        packages = {int(p.name.rsplit("_", 1)[1]): p for p in paths["pkgs"]}
        blocks: dict[int, bytes] = {}
        for res in manifest.res:
            if res.nh != level_hash or res.th not in wanted:
                continue
            if res.bi not in blocks:
                blk = manifest.blks[res.bi]
                with open(packages[blk.pi], "rb") as fh:
                    fh.seek(blk.off)
                    compressed = fh.read(blk.csz)
                blocks[res.bi] = rad.zstandard.ZstdDecompressor().decompress(compressed, max_output_size=blk.dsz + 65536)
            found[res.th].append(blocks[res.bi][res.off:res.off + res.rsz])
    out = {}
    for th in type_hashes:
        if len(found[th]) != 1:
            raise StockMissing(f"type 0x{th:016x} of level 0x{level_hash:016x}: {len(found[th])} resources under {data_dir}, expected exactly 1")
        out[th] = found[th][0]
    return out


def data_dir_from(arg: str | None) -> Path:
    value = arg or os.environ.get(DATA_DIR_ENV)
    if not value:
        raise UsageError(f"no game data directory: pass --data-dir <the game's _data/<id> folder> or set {DATA_DIR_ENV}")
    path = Path(value)
    if not path.is_dir():
        raise UsageError(f"--data-dir / {DATA_DIR_ENV} {value} is not a directory")
    return path


def stock_for(table: dict, data_dir: Path) -> dict[str, bytes]:
    level = int(table["level"], 16)
    by_type = read_stock(data_dir, level, [int(e["stock_type"], 16) for e in table["files"]])
    return {e["override_type"]: by_type[int(e["stock_type"], 16)] for e in table["files"]}


def generate(table: dict, stocks: dict[str, bytes], out_dir: Path, data_dir: Path | None = None, force: bool = False) -> list[Path]:
    """Write every file of the set into `out_dir`. Nothing is written unless every file builds."""
    out_dir = out_dir.resolve()
    if data_dir is not None and (out_dir == data_dir.resolve() or data_dir.resolve() in out_dir.parents):
        raise UsageError(f"--out {out_dir} is inside the game data directory {data_dir}: the install is never written")
    built = {}
    for entry in table["files"]:
        if entry["override_type"] not in stocks:
            raise StockMissing(f"no stock resource given for {entry['override_type']}")
        built[file_name(entry, table["level"])] = build_file(entry, stocks[entry["override_type"]])
    existing = [n for n in built if (out_dir / n).exists()]
    if existing and not force:
        raise UsageError(f"{out_dir} already has {existing[0]} (and {len(existing) - 1} more): pass --force to replace them")
    out_dir.mkdir(parents=True, exist_ok=True)
    paths = []
    for name, data in built.items():
        (out_dir / name).write_bytes(data)
        paths.append(out_dir / name)
    return paths


def derive_ops(stock: bytes, target: bytes, min_copy: int = MIN_COPY) -> list:
    """An edit list that turns `stock` into `target`: copies of stock runs, and puts for what stock cannot supply."""
    index = collections.defaultdict(list)
    for i in range(len(stock) - min_copy + 1):
        index[stock[i:i + min_copy]].append(i)
    ops: list = []
    pending = bytearray()
    expect = 0  # where the previous copy ended in stock: of equal runs, the nearest to it is taken
    j = 0
    while j < len(target):
        best = None
        for c in index.get(target[j:j + min_copy], []):
            n = 0
            while c + n < len(stock) and j + n < len(target) and stock[c + n] == target[j + n]:
                n += 1
            key = (n, -abs(c - expect))
            if best is None or key > best[0]:
                best = (key, c, n)
        if best is not None:
            if pending:
                ops.append(["p", bytes(pending).hex()])
                pending = bytearray()
            ops.append(["c", best[1], best[2]])
            expect = best[1] + best[2]
            j += best[2]
        else:
            pending.append(target[j])
            j += 1
    if pending:
        ops.append(["p", bytes(pending).hex()])
    return ops


def derive(table: dict, stocks: dict[str, bytes], reference: Path) -> dict:
    files = []
    for entry in table["files"]:
        name = file_name(entry, table["level"])
        target = (reference / name).read_bytes()
        stock = stocks[entry["override_type"]]
        ops = derive_ops(stock, target)
        assert apply_ops(stock, ops) == target
        files.append({**entry, "stock_size": len(stock), "stock_sha256": sha256(stock), "size": len(target), "sha256": sha256(target), "ops": ops})
    return {**table, "files": files}


def dump_table(table: dict) -> str:
    """One line per file's edit list so a changed card is a readable diff."""
    lines = ["{", f'  "schema": {table["schema"]},', f'  "level": {json.dumps(table["level"])},', '  "files": [']
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
        table = load_table(args.patches)
        stocks = stock_for(table, data_dir)
        if args.command == "generate":
            paths = generate(table, stocks, args.out, data_dir, args.force)
            for path in paths:
                print(f"wrote {path.name} {path.stat().st_size} bytes sha256 {sha256(path.read_bytes())}")
            print(f"plugins_override_generator: wrote {len(paths)} files to {args.out}")
        else:
            args.out.write_text(dump_table(derive(table, stocks, args.reference)))
            print(f"plugins_override_generator: wrote {args.out}")
    except OverrideError as exc:
        print(f"plugins_override_generator: refused: {exc}", file=sys.stderr)
        return EXIT_REFUSED
    return 0


if __name__ == "__main__":
    sys.exit(main())
