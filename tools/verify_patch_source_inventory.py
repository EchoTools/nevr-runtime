#!/usr/bin/env python3
"""Every ``patch/*.cpp`` on disk is in the CMake ``PATCHES_SOURCES`` list once, and every entry exists (#1350).

The CMake list is the only list: a PR that adds a runtime patch edits it and adds the file, and nothing else.
"""

import argparse
from collections import Counter
from pathlib import Path
import re
import sys


REPO = Path(__file__).resolve().parent.parent
DEFAULT_CMAKE = REPO / "src/runtime/CMakeLists.txt"

class InventoryError(Exception):
    """The authoritative CMake list could not be read as an inventory."""


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--cmake",
        type=Path,
        default=DEFAULT_CMAKE,
        help="authoritative runtime CMakeLists.txt (default: %(default)s)",
    )
    return parser.parse_args()


def patches_sources_block(cmake: Path) -> str:
    try:
        text = cmake.read_text(encoding="utf-8")
    except OSError as error:
        raise InventoryError(f"cannot read {cmake}: {error}") from error

    match = re.search(
        r"^\s*set\s*\(\s*PATCHES_SOURCES\b(.*?)^\s*\)", text, re.MULTILINE | re.DOTALL
    )
    if match is None:
        raise InventoryError(f"PATCHES_SOURCES list not found in {cmake}")
    return match.group(1)


def patch_sources(cmake: Path) -> list[str]:
    entries = []
    for line in patches_sources_block(cmake).splitlines():
        value = line.split("#", maxsplit=1)[0].strip()
        match = re.fullmatch(r'"([^"\n]+)"', value)
        if match is not None:
            entries.append(match.group(1))
    return [entry for entry in entries if entry.startswith("patch/") and entry.endswith(".cpp")]


def verify(cmake: Path) -> list[str]:
    errors = []
    actual = patch_sources(cmake)
    actual_counts = Counter(actual)
    actual_set = set(actual)
    for source in sorted(source for source, count in actual_counts.items() if count > 1):
        errors.append(f"duplicate PATCHES_SOURCES patch entry: {source}")
    for source in sorted(actual_set):
        if not (cmake.parent / source).is_file():
            errors.append(f"PATCHES_SOURCES patch source is missing from disk: {source}")
    on_disk = {f"patch/{path.name}" for path in (cmake.parent / "patch").glob("*.cpp")}
    for source in sorted(on_disk - actual_set):
        errors.append(f"patch source on disk is not in PATCHES_SOURCES: {source}")
    return errors


def main() -> int:
    cmake = parse_args().cmake
    try:
        errors = verify(cmake)
    except InventoryError as error:
        print(f"patch-source-inventory: FAIL {error}", file=sys.stderr)
        return 1

    if errors:
        for error in errors:
            print(f"patch-source-inventory: FAIL {error}", file=sys.stderr)
        return 1

    sources = patch_sources(cmake)
    print(f"patch-source-inventory: OK count={len(sources)} names={', '.join(sources)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
