#!/usr/bin/env python3
"""Pin the runtime patch implementation inventory to its CMake source list.

The CMake ``PATCHES_SOURCES`` list is authoritative for compiled runtime
sources.  This checker deliberately reads only that list, then narrows it to
``patch/*.cpp`` implementation entries; it does not infer an inventory from a
filesystem glob.
"""

import argparse
from collections import Counter
from pathlib import Path
import re
import sys


REPO = Path(__file__).resolve().parent.parent
DEFAULT_CMAKE = REPO / "src/runtime/CMakeLists.txt"

# Updating this list is an intentional review point: adding or removing a
# runtime patch must update the expected inventory in the same reviewed change.
EXPECTED_PATCH_SOURCES = (
    "patch/asset_cdn.cpp",
    "patch/binary_bug_fixes.cpp",
    "patch/broadcaster_guard.cpp",
    "patch/broadcaster_hook_stats.cpp",
    "patch/headless_graphics.cpp",
    "patch/mode_patches.cpp",
    "patch/pnsrad_enabler.cpp",
    "patch/resource_override.cpp",
    "patch/xpid_patch.cpp",
)
EXPECTED_PATCH_SOURCE_COUNT = 9


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
    if len(EXPECTED_PATCH_SOURCES) != EXPECTED_PATCH_SOURCE_COUNT:
        errors.append(
            "checker bug: EXPECTED_PATCH_SOURCE_COUNT does not match "
            "EXPECTED_PATCH_SOURCES"
        )

    actual = patch_sources(cmake)
    actual_counts = Counter(actual)
    expected = set(EXPECTED_PATCH_SOURCES)
    actual_set = set(actual)

    for source in sorted(source for source, count in actual_counts.items() if count > 1):
        errors.append(f"duplicate PATCHES_SOURCES patch entry: {source}")
    for source in sorted(actual_set - expected):
        errors.append(f"unexpected PATCHES_SOURCES patch source: {source}")
    for source in sorted(expected - actual_set):
        errors.append(f"expected patch source absent from PATCHES_SOURCES: {source}")
    for source in sorted(actual_set):
        if not (cmake.parent / source).is_file():
            errors.append(f"PATCHES_SOURCES patch source is missing from disk: {source}")
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
        print(
            "patch-source-inventory: review any runtime patch addition/removal and update "
            "EXPECTED_PATCH_SOURCES deliberately.",
            file=sys.stderr,
        )
        return 1

    names = ", ".join(EXPECTED_PATCH_SOURCES)
    print(
        f"patch-source-inventory: OK count={EXPECTED_PATCH_SOURCE_COUNT} names={names}"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
