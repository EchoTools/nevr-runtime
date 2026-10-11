#!/usr/bin/env python3
"""Fail `just verify` when the runtime GTest declarations fall below the floor.

Counts the lines of src/runtime/tests/*.cpp that begin `TEST(` or `TEST_F(`: exactly what the gate
counted with `grep -hE '^TEST(_F)?\\(' src/runtime/tests/*.cpp | wc -l` (an indented, commented or
TEST_P declaration is not counted). The floor lives in the justfile next to the call and is meant to
sit at the real count: a test that disappears must fail the gate, and a test that is added raises
the floor in the same change (tools/tests/test_verify_gtest_floor.py pins both).
"""

import argparse
import re
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
DEFAULT_DIR = REPO / "src" / "runtime" / "tests"
DECLARATION = re.compile(r"^TEST(_F)?\(")


def count_declarations(directory: Path) -> int:
    total = 0
    for path in sorted(directory.glob("*.cpp")):
        with path.open(encoding="utf-8", errors="replace") as handle:
            total += sum(1 for line in handle if DECLARATION.match(line))
    return total


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--floor", type=int, required=True)
    parser.add_argument("--dir", type=Path, default=DEFAULT_DIR)
    args = parser.parse_args()

    if not args.dir.is_dir() or not any(args.dir.glob("*.cpp")):
        print(f"verify: FAIL — no runtime test sources in {args.dir}; the GTest count cannot be taken.",
              file=sys.stderr)
        return 1
    count = count_declarations(args.dir)
    if count < args.floor:
        print(f"verify: FAIL — runtime GTest count fell to {count} (floor {args.floor}).", file=sys.stderr)
        return 1
    print(f"verify: runtime GTest declarations={count} (floor {args.floor})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
