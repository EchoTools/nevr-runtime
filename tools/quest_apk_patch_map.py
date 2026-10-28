#!/usr/bin/env python3
"""Check tools/quest_apk_patch_map.tsv: every change the community patch repo applies to the Quest APK, mapped to what
our build does instead (#1187).

Without --repo (run by `just test-tools`): every row has 7 fields and a reason; its status is done, not_needed, open
or table; a done row names our code as `<path>:<symbol>` (the path exists in this tree and holds the symbol as a
word, so a renamed or deleted hook fails the table); an open row names an issue (`#<n>`); a table row names
tools/quest_patcher_sites.tsv; no (source, site) appears twice; the header pins the patch repo's commit.

With --repo <checkout of the patch repo> (run by `just test-quest-apk-patch-map`): the checkout is at the pinned
commit; every tracked file is the source of a row and every row's source is a tracked file; every address of an
enabled feature in an applied YAML (`*.so.yaml`, the files the patch repo's patch.py applies; the libpnsovr one
excepted) has a row with that feature's name, every disabled feature has a not_needed row, and no row names an address
the YAML does not have; every address of the libpnsovr YAMLs is a row of tools/quest_patcher_sites.tsv; every
`@<text>` site is in its file and every other address site of a script is written in it; the stock libraries' build
IDs are our pinned ones.

Exit 0 when every check passes; 1 with one line per failure otherwise.
"""

import argparse
import re
import struct
import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
TABLE = REPO / "tools" / "quest_apk_patch_map.tsv"
PATCHER_SITES = "tools/quest_patcher_sites.tsv"
FIELDS = ("source", "site", "feature", "change", "status", "ours", "reason")
STATUSES = ("done", "not_needed", "open", "table")
PIN = re.compile(r"^#@ echoApkPatch ([0-9a-f]{40})$")
# The YAMLs whose sites are rows of tools/quest_patcher_sites.tsv (their_va, the second column).
PATCHER_SITE_YAMLS = ("configs/libpnsovr.so.yaml", "patcher/license_patch.yaml")
# Each stock library and the header constant holding our pinned build ID for it.
BUILD_IDS = (
    ("binary/source/libr15.so", "src/quest/sentinel/pinned_targets.h", "kLibR15BuildId"),
    ("binary/source/libpnsradmatchmaking.so", "src/quest/sentinel/pinned_targets.h", "kMatchmakingBuildId"),
    ("binary/source/libpnsovr.so", "src/quest/login/login_prerequisite_targets.h", "kPnsovrBuildId"),
)


def read_table(path: Path) -> tuple:
    """(the pinned commit or None, the rows as dicts with their line number under "line")."""
    pin = None
    rows = []
    for number, line in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
        match = PIN.match(line)
        if match:
            pin = match.group(1)
            continue
        if not line.strip() or line.startswith("#"):
            continue
        parts = line.split("\t")
        row = dict(zip(FIELDS, parts))
        row["line"] = number
        row["fields"] = len(parts)
        rows.append(row)
    return pin, rows


def site_address(site: str):
    """The site as an int when it is an address, else None."""
    return int(site, 16) if re.fullmatch(r"0x[0-9a-fA-F]+", site) else None


def site_key(row: dict) -> tuple:
    address = site_address(row["site"])
    if address is not None:
        return row["source"], address
    if row["site"] == "feature":
        return row["source"], "feature", row["feature"]
    return row["source"], row["site"]


def check_ours(row: dict, root: Path) -> list:
    """Failures of one row's `ours` against its status."""
    where = f"line {row['line']}"
    status = row["status"]
    items = [item.strip() for item in row["ours"].split(",")]
    failures = []
    hooks = issues = 0
    for item in items:
        if item == "-":
            continue
        if re.fullmatch(r"#[0-9]+", item):
            issues += 1
            continue
        if status == "table":
            continue
        path, _, symbol = item.partition(":")
        target = root / path
        if not symbol or not target.is_file():
            failures.append(f"{where}: {item!r} does not name an existing <path>:<symbol> or an issue")
            continue
        if not re.search(rf"(?<![A-Za-z0-9_]){re.escape(symbol)}(?![A-Za-z0-9_])", target.read_text(encoding="utf-8")):
            failures.append(f"{where}: {symbol} not found in {path}")
            continue
        hooks += 1
    if status == "done" and not hooks:
        failures.append(f"{where}: done but names no <path>:<symbol> of ours")
    if status == "open" and not issues:
        failures.append(f"{where}: open but names no issue")
    if status == "table" and items != [PATCHER_SITES]:
        failures.append(f"{where}: a table row names {PATCHER_SITES} and nothing else")
    if status == "table" and not (root / PATCHER_SITES).is_file():
        failures.append(f"{where}: {PATCHER_SITES} is not in the tree")
    return failures


def check_rows(pin, rows: list, root: Path) -> list:
    """Failures of the table on its own (no patch repo needed)."""
    failures = []
    if pin is None:
        failures.append("no '#@ echoApkPatch <commit>' line pins the patch repo's commit")
    seen = {}
    for row in rows:
        where = f"line {row['line']}"
        if row["fields"] != len(FIELDS):
            failures.append(f"{where}: {row['fields']} fields, want {len(FIELDS)}")
            continue
        for name in ("source", "site", "feature", "change", "reason"):
            if row[name].strip() in ("", "-") and not (name == "site" and row[name] == "-"):
                failures.append(f"{where}: empty {name}")
        if row["status"] not in STATUSES:
            failures.append(f"{where}: status {row['status']!r} is not one of {', '.join(STATUSES)}")
            continue
        if not row["ours"].strip():
            failures.append(f"{where}: empty ours (write - for none)")
            continue
        failures += check_ours(row, root)
        key = site_key(row)
        if key in seen:
            failures.append(f"{where}: {row['source']} {row['site']} already at line {seen[key]}")
        seen[key] = row["line"]
    return failures


def yaml_features(text: str) -> list:
    """The features of a patch YAML, in order: {"name", "enabled", "addresses"}. Only the three keys the check needs
    are read, by their lines; patch.py's own reader is the authority on the format."""
    features = []
    feature = None
    for line in text.splitlines():
        match = re.match(r'^- feature_name:\s*"(.*)"\s*$', line)
        if match:
            feature = {"name": match.group(1), "enabled": None, "addresses": []}
            features.append(feature)
            continue
        if feature is None:
            continue
        match = re.match(r"^  enabled:\s*(true|false)\s*$", line)
        if match:
            feature["enabled"] = match.group(1) == "true"
            continue
        match = re.match(r"^\s+- address:\s*(0x[0-9a-fA-F]+)\s*$", line)
        if match:
            feature["addresses"].append(int(match.group(1), 16))
    return features


def gnu_build_id(data: bytes):
    """The NT_GNU_BUILD_ID of a 64-bit little-endian ELF, as hex, or None."""
    if data[:4] != b"\x7fELF" or data[4] != 2 or data[5] != 1:
        return None
    shoff = struct.unpack_from("<Q", data, 0x28)[0]
    shentsize, shnum = struct.unpack_from("<HH", data, 0x3A)
    for index in range(shnum):
        header = shoff + index * shentsize
        sh_type = struct.unpack_from("<I", data, header + 4)[0]
        if sh_type != 7:  # SHT_NOTE
            continue
        offset, size = struct.unpack_from("<QQ", data, header + 0x18)
        pos = offset
        while pos + 12 <= offset + size:
            namesz, descsz, note_type = struct.unpack_from("<III", data, pos)
            name = data[pos + 12:pos + 12 + namesz]
            desc_at = pos + 12 + ((namesz + 3) & ~3)
            if note_type == 3 and name == b"GNU\0":  # NT_GNU_BUILD_ID
                return data[desc_at:desc_at + descsz].hex()
            pos = desc_at + ((descsz + 3) & ~3)
    return None


def pinned_constant(root: Path, header: str, name: str):
    match = re.search(rf'\b{name}\s*=\s*"([0-9a-f]+)"', (root / header).read_text(encoding="utf-8"))
    return match.group(1) if match else None


def git(repo: Path, *args: str) -> str:
    return subprocess.run(["git", "-C", str(repo), *args], check=True, capture_output=True, text=True).stdout


def check_repo(pin, rows: list, repo: Path, root: Path) -> list:
    """Failures of the table against a checkout of the patch repo."""
    failures = []
    head = git(repo, "rev-parse", "HEAD").strip()
    if head != pin:
        failures.append(f"the patch repo is at {head}, the table was read at {pin}")
    tracked = set(git(repo, "ls-files").splitlines())
    rows = [r for r in rows if r["fields"] == len(FIELDS)]
    sources = {r["source"] for r in rows}
    for path in sorted(tracked - sources):
        failures.append(f"{path} is tracked in the patch repo and is the source of no row")
    for path in sorted(sources - tracked):
        failures.append(f"{path} is the source of a row and is not tracked in the patch repo")

    applied = sorted(p for p in tracked if p.endswith(".so.yaml") and p not in PATCHER_SITE_YAMLS)
    for path in applied:
        features = yaml_features((repo / path).read_text(encoding="utf-8"))
        if not features:
            failures.append(f"{path}: no feature read")
        mine = [r for r in rows if r["source"] == path]
        by_address = {site_address(r["site"]): r for r in mine if site_address(r["site"]) is not None}
        disabled = {r["feature"]: r for r in mine if r["site"] == "feature"}
        wanted = set()
        for feature in features:
            if feature["enabled"] is None:
                failures.append(f"{path}: feature {feature['name']!r} has no enabled line")
                continue
            if not feature["enabled"]:
                row = disabled.get(feature["name"])
                if row is None:
                    failures.append(f"{path}: disabled feature {feature['name']!r} has no row")
                elif row["status"] != "not_needed":
                    failures.append(f"line {row['line']}: disabled feature {feature['name']!r} is {row['status']}, "
                                    "want not_needed")
                continue
            for address in feature["addresses"]:
                wanted.add(address)
                row = by_address.get(address)
                if row is None:
                    failures.append(f"{path}: {address:#x} ({feature['name']}) has no row")
                elif row["feature"] != feature["name"]:
                    failures.append(f"line {row['line']}: {address:#x} is in feature {feature['name']!r}, the table "
                                    f"says {row['feature']!r}")
        for address, row in by_address.items():
            if address not in wanted:
                failures.append(f"line {row['line']}: {path} applies no {address:#x}")
        names = {f["name"] for f in features if f["enabled"] is False}
        for name, row in disabled.items():
            if name not in names:
                failures.append(f"line {row['line']}: {path} has no disabled feature {name!r}")

    patcher_sites = {int(line.split("\t")[1], 16)
                     for line in (root / PATCHER_SITES).read_text(encoding="utf-8").splitlines()
                     if line.strip() and not line.startswith("#")}
    for path in PATCHER_SITE_YAMLS:
        for feature in yaml_features((repo / path).read_text(encoding="utf-8")):
            for address in feature["addresses"]:
                if address not in patcher_sites:
                    failures.append(f"{path}: {address:#x} is not a row of {PATCHER_SITES}")

    for row in rows:
        if row["source"] in applied or row["source"] not in tracked:
            continue
        text = (repo / row["source"]).read_text(encoding="utf-8", errors="replace")
        site = row["site"]
        if site.startswith("@") and site[1:] not in text:
            failures.append(f"line {row['line']}: {site[1:]!r} is not in {row['source']}")
        address = site_address(site)
        if address is not None and not re.search(rf"0x0*{address:x}\b", text, re.IGNORECASE):
            failures.append(f"line {row['line']}: {row['source']} does not write {site}")

    for path, header, name in BUILD_IDS:
        found = gnu_build_id((repo / path).read_bytes())
        ours = pinned_constant(root, header, name)
        if ours is None:
            failures.append(f"{name} not found in {header}")
        elif found != ours:
            failures.append(f"{path} has build ID {found}, {header} pins {name} = {ours}")
    return failures


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--table", type=Path, default=TABLE)
    parser.add_argument("--root", type=Path, default=REPO, help="the tree our code is looked up in")
    parser.add_argument("--repo", type=Path, help="a checkout of the patch repo, to check the table against it")
    args = parser.parse_args()
    pin, rows = read_table(args.table)
    failures = check_rows(pin, rows, args.root)
    if args.repo is not None and pin is not None:
        failures += check_repo(pin, rows, args.repo, args.root)
    for failure in failures:
        print(f"quest_apk_patch_map: FAIL {failure}")
    if failures:
        return 1
    counts = {status: sum(1 for r in rows if r["status"] == status) for status in STATUSES}
    checked = f"against the patch repo at {pin}" if args.repo else "table only"
    print(f"quest_apk_patch_map: OK {len(rows)} rows: " + ", ".join(f"{n} {s}" for s, n in counts.items())
          + f" ({checked})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
