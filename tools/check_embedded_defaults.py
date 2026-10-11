#!/usr/bin/env python3
"""Fail unless the defaults embedded in a build equal config/public-defaults.env.

usage: check_embedded_defaults.py --header <build>/generated/nevr_builtin_defaults.h [--binary <file>]...
                                  [--defaults config/public-defaults.env]

The build reads only that file (cmake/nevr_builtin_defaults.cmake). This proves it: the
generated header holds exactly the file's four values, and every --binary (the PC DLL, the Quest
sentinel) contains each value as a string. Reports key names, never values.
"""

import argparse
import re
import sys
from pathlib import Path

KEYS = {
    "NEVR_SOCKET_URI": "kSocketUri",
    "NEVR_HTTP_URI": "kHttpUri",
    "NEVR_PUBLIC_API_KEY": "kPublicApiKey",
    "NEVR_PUBLIC_SOCKET_KEY": "kPublicSocketKey",
}


def read_defaults(path: Path) -> dict:
    values = {}
    for line in path.read_text(encoding="utf-8").splitlines():
        stripped = line.strip()
        if not stripped or stripped.startswith("#"):
            continue
        key, sep, value = stripped.partition("=")
        if not sep or key not in KEYS or key in values or not value:
            raise ValueError(f"{path}: bad or repeated line for key {key or '?'}")
        values[key] = value
    missing = sorted(set(KEYS) - set(values))
    if missing:
        raise ValueError(f"{path}: missing keys {missing}")
    return values


def read_header(path: Path) -> dict:
    text = path.read_text(encoding="utf-8")
    out = {}
    for key, name in KEYS.items():
        match = re.search(rf'\b{name}\s*=\s*"([^"]*)";', text)
        if not match:
            raise ValueError(f"{path}: {name} not found")
        out[key] = match.group(1)
    return out


def check(defaults: Path, header: Path, binaries: list) -> list:
    problems = []
    want = read_defaults(defaults)
    have = read_header(header)
    for key in KEYS:
        if have[key] != want[key]:
            problems.append(f"{key}: the generated header differs from {defaults.name}")
    for binary in binaries:
        data = binary.read_bytes()
        for key in KEYS:
            if want[key].encode("utf-8") not in data:
                problems.append(f"{key}: not embedded in {binary.name}")
    return problems


def main(argv=None) -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--defaults", default="config/public-defaults.env", type=Path)
    parser.add_argument("--header", required=True, type=Path)
    parser.add_argument("--binary", action="append", default=[], type=Path)
    args = parser.parse_args(argv)
    try:
        problems = check(args.defaults, args.header, args.binary)
    except (OSError, ValueError) as error:
        print(f"check_embedded_defaults: FAIL: {error}", file=sys.stderr)
        return 1
    if problems:
        for problem in problems:
            print(f"check_embedded_defaults: FAIL: {problem}", file=sys.stderr)
        return 1
    print(f"check_embedded_defaults: OK ({len(KEYS)} keys; header and {len(args.binary)} binary file(s) match {args.defaults.name})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
