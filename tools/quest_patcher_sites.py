#!/usr/bin/env python3
"""Check tools/quest_patcher_sites.tsv: the community patch repo's libpnsovr.so edits mapped to our hooks (#944).

Without --lib (run by `just test-tools`): every row has 8 fields, its vaddr is its their_va - 0x100000, no vaddr
appears twice, and every row names a hook or gives a reason. A named hook is `<path>:<symbol>`; the path must exist
in this tree and contain the symbol as a word, so a renamed or deleted hook fails the table.

With --lib <libpnsovr.so> (run by `just test-quest-hooks-pinned` on the library extracted from the pinned APK): every
row's stock word is the word the library holds at its vaddr, and its function is the exported function (or the PLT
stub) that holds it.

Exit 0 when every check passes; 1 with one line per failure otherwise.
"""

import argparse
import re
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
TABLE = REPO / "tools" / "quest_patcher_sites.tsv"
DISASM_OFFSET = 0x100000
FIELDS = ("source", "their_va", "vaddr", "stock", "function", "change", "covered_by", "reason")


def read_table(path: Path) -> list:
    """The rows as dicts, each with its line number under "line"."""
    rows = []
    for number, line in enumerate(path.read_text().splitlines(), 1):
        if not line.strip() or line.startswith("#"):
            continue
        parts = line.split("\t")
        row = dict(zip(FIELDS, parts))
        row["line"] = number
        row["fields"] = len(parts)
        rows.append(row)
    return rows


def check_rows(rows: list, root: Path) -> list:
    """Failures of the table on its own (no library needed)."""
    failures = []
    seen = {}
    for row in rows:
        where = f"line {row['line']}"
        if row["fields"] != len(FIELDS):
            failures.append(f"{where}: {row['fields']} fields, want {len(FIELDS)}")
            continue
        try:
            theirs = int(row["their_va"], 16)
            ours = int(row["vaddr"], 16)
            int(row["stock"], 16)
        except ValueError:
            failures.append(f"{where}: their_va, vaddr and stock must be hex")
            continue
        if ours != theirs - DISASM_OFFSET:
            failures.append(f"{where}: vaddr {ours:#x} is not their_va {theirs:#x} - {DISASM_OFFSET:#x}")
        if ours in seen:
            failures.append(f"{where}: vaddr {ours:#x} already at line {seen[ours]}")
        seen[ours] = row["line"]
        for name in ("source", "function", "change"):
            if not row[name].strip() or row[name].strip() == "-":
                failures.append(f"{where}: empty {name}")
        covered = row["covered_by"].strip()
        reason = row["reason"].strip()
        if covered in ("", "-") and reason in ("", "-"):
            failures.append(f"{where}: {row['source']} {ours:#x} has neither a hook nor a reason")
        if covered not in ("", "-"):
            for hook in covered.split(","):
                path, _, symbol = hook.strip().partition(":")
                target = root / path
                if not symbol or not target.is_file():
                    failures.append(f"{where}: hook {hook.strip()!r} does not name an existing <path>:<symbol>")
                    continue
                if not re.search(rf"\b{re.escape(symbol)}\b", target.read_text()):
                    failures.append(f"{where}: {symbol} not found in {path}")
    return failures


def check_library(rows: list, lib: Path) -> list:
    """Failures of the rows against the real library."""
    sys.path.insert(0, str(REPO / "tools"))
    from pinned_ovr_import_walk import Elf

    elf = Elf(lib.read_bytes())
    failures = []
    for row in rows:
        if row["fields"] != len(FIELDS):
            continue
        ours = int(row["vaddr"], 16)
        try:
            word = elf.word(ours)
        except KeyError:
            failures.append(f"line {row['line']}: {ours:#x} is not in the library")
            continue
        if word != int(row["stock"], 16):
            failures.append(f"line {row['line']}: {ours:#x} holds {word:08x}, the table says {row['stock']}")
        stub = elf.plt_symbols.get(ours & ~0xF)
        function = f"{stub}@plt" if stub else elf.enclosing_function(ours)
        if function != row["function"]:
            failures.append(f"line {row['line']}: {ours:#x} is in {function}, the table says {row['function']}")
    return failures


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--table", type=Path, default=TABLE)
    parser.add_argument("--root", type=Path, default=REPO, help="the tree the hooks are looked up in")
    parser.add_argument("--lib", type=Path, help="the pinned libpnsovr.so, to check stock words and functions")
    args = parser.parse_args()
    rows = read_table(args.table)
    failures = check_rows(rows, args.root)
    if args.lib is not None:
        failures += check_library(rows, args.lib)
    for failure in failures:
        print(f"quest_patcher_sites: FAIL {failure}")
    if failures:
        return 1
    covered = sum(1 for r in rows if r.get("covered_by", "-").strip() not in ("", "-"))
    checked = "stock words and functions match the library" if args.lib else "table only"
    print(f"quest_patcher_sites: OK {len(rows)} rows, {covered} with a hook, {len(rows) - covered} with a reason only "
          f"({checked})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
