#!/usr/bin/env python3
"""Judge one GTest binary's run from the XML that `--gtest_output=xml:<file>` wrote (#2152).

A process that exits 0 has not thereby passed: under Wine a test process that overflowed its stack exited 0 with no
GTest summary. The verdict is GTest's own: a result file with at least one test run and no failures and no errors.

Usage: gtest_verdict.py <xml file>
Exit 0 and one line `ok <n> tests` when the run passed; exit 1 and one line naming the reason otherwise.
"""
import sys
import xml.etree.ElementTree as ET

NO_TESTS = 0


def verdict(path: str) -> tuple:
    """(passed, the reason in words)."""
    try:
        root = ET.parse(path).getroot()
    except (OSError, ET.ParseError) as error:
        return False, f"result unreadable: {type(error).__name__}"
    if root.tag != "testsuites":
        return False, f"not a GTest result (root element {root.tag!r})"
    try:
        tests = int(root.get("tests", "0"))
        failures = int(root.get("failures", "0"))
        errors = int(root.get("errors", "0"))
    except ValueError:
        return False, "result counts are not numbers"
    if tests <= NO_TESTS:
        return False, "no tests ran"
    if failures or errors:
        return False, f"{failures} failed, {errors} errors of {tests} tests"
    return True, f"{tests} tests"


def main(argv: list) -> int:
    if len(argv) != 2:
        print("usage: gtest_verdict.py <xml file>", file=sys.stderr)
        return 2
    passed, reason = verdict(argv[1])
    print(("ok " if passed else "no pass: ") + reason)
    return 0 if passed else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv))
