"""Reads tools/test_shelf.json for the Python half of the repo's test runners (the Go half is tools/internal/testshelf).

ADR 0044 recorded reason: tools/run_tool_tests.py and tools/tests/conftest.py are Python and read the shelf in-process, and the
justfile reads the ceiling through `python3 tools/shelf_reader.py --ceiling`, so one reader serves the Python runner and the recipes.

The shelf holds the ceiling every test runner of the repo holds a test to (`ceiling_s`) and the tests that are over it
(`entries`). `skip_reason` is what keeps a shelved pytest test out of a run: tools/tests/conftest.py marks the test
skipped with that text, so the skip is generated from the shelf and no test file carries a hand-written skip for it.

    shelf_reader.py --ceiling                 the ceiling in whole seconds
    shelf_reader.py --gtest-filter BINARY     the gtest filter that keeps BINARY's shelved tests out ("" when none)
"""

import json
import pathlib
import sys

SHELF_PATH = pathlib.Path(__file__).resolve().parent / "test_shelf.json"
SCHEMA = 1
MAX_CEILING_S = 60
KINDS = ("just", "target", "pytest", "go", "gtest", "ctest")
KINDS_WITH_CASES = ("pytest", "go", "gtest")


class ShelfError(Exception):
    """The shelf cannot be read or is not well formed."""


def split_id(entry_id: str):
    """(kind, unit, case) of an id `<kind>:<unit>` or `<kind>:<unit>#<case>`; case is "" for a whole unit."""
    kind, sep, rest = entry_id.partition(":")
    if not sep or kind not in KINDS or not rest:
        raise ShelfError(f"id {entry_id!r} is not <kind>:<unit> with a kind of {', '.join(KINDS)}")
    unit, has_case, case = rest.partition("#")
    if not unit or (has_case and not case):
        raise ShelfError(f"id {entry_id!r} has an empty unit or an empty case")
    if has_case and kind not in KINDS_WITH_CASES:
        raise ShelfError(f"id {entry_id!r}: a {kind} has no cases")
    return kind, unit, case


def load(path=SHELF_PATH) -> dict:
    """The shelf at `path`, checked: its schema, a ceiling of 1 to 60 s, and for every entry a known id, a reason, a
    measured-where and a date, and no id twice. Raises ShelfError."""
    try:
        data = json.loads(pathlib.Path(path).read_text(encoding="utf-8"))
    except (OSError, ValueError) as exc:
        raise ShelfError(f"test shelf {path}: {exc}") from exc
    if not isinstance(data, dict) or data.get("schema") != SCHEMA:
        raise ShelfError(f"test shelf {path}: schema is not {SCHEMA}")
    ceiling = data.get("ceiling_s")
    if not isinstance(ceiling, int) or isinstance(ceiling, bool) or not 1 <= ceiling <= MAX_CEILING_S:
        raise ShelfError(f"test shelf {path}: ceiling_s {ceiling!r} is not a whole number of seconds from 1 to {MAX_CEILING_S}")
    seen = set()
    for entry in data.get("entries", []):
        split_id(entry.get("id", ""))
        if entry["id"] in seen:
            raise ShelfError(f"test shelf {path}: id {entry['id']} is listed twice")
        seen.add(entry["id"])
        for key in ("reason", "measured", "date"):
            if not str(entry.get(key, "")).strip():
                raise ShelfError(f"test shelf {path}: {entry['id']} has no {key}")
    return data


def ceiling_s(path=SHELF_PATH) -> int:
    return load(path)["ceiling_s"]


def skip_reason(shelf: dict, unit: str, class_name, test_name) -> "str | None":
    """The text a shelved pytest test is skipped with, or None when the test is not shelved. `unit` is the test file's stem,
    `class_name` the test's class (or None) and `test_name` its function name; the shelf may name the whole file, the
    class (`Class`) or the test (`Class.test`, or `test` for a function outside a class)."""
    candidates = [f"pytest:{unit}"]
    if class_name:
        candidates += [f"pytest:{unit}#{class_name}", f"pytest:{unit}#{class_name}.{test_name}"]
    else:
        candidates.append(f"pytest:{unit}#{test_name}")
    for entry in shelf.get("entries", []):
        if entry["id"] in candidates:
            return f"shelved {entry['date']}: {entry['reason']} (tools/test_shelf.json: {entry['id']})"
    return None


def gtest_filter(shelf: dict, binary: str, base: str = "") -> str:
    """The `--gtest_filter` value that runs `base` (everything when empty) without the binary's shelved tests; "" when the
    binary has none shelved and `base` is empty. A base that already has a negative part gets the shelved tests added to it."""
    shelved = []
    for entry in shelf.get("entries", []):
        kind, unit, case = split_id(entry["id"])
        if kind == "gtest" and unit == binary and case:
            shelved.append(case)
    if not shelved:
        return base
    negative = ":".join(sorted(shelved))
    if not base:
        return "-" + negative
    return f"{base}:{negative}" if "-" in base else f"{base}-{negative}"


def main(argv: list) -> int:
    try:
        shelf = load()
        if argv == ["--ceiling"]:
            print(shelf["ceiling_s"])
            return 0
        if len(argv) in (2, 3) and argv[0] == "--gtest-filter":
            print(gtest_filter(shelf, argv[1], argv[2] if len(argv) == 3 else ""))
            return 0
    except ShelfError as exc:
        print(f"shelf_reader: FAIL {exc}", file=sys.stderr)
        return 2
    print("usage: shelf_reader.py --ceiling | --gtest-filter BINARY [BASE_FILTER]", file=sys.stderr)
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
