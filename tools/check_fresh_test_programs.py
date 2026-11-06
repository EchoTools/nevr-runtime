#!/usr/bin/env python3
"""Fail a run that executes a test program its build did not declare or did not make (#2069).

A reused build tree keeps the executables of earlier builds. When a configure removes a target, its executable
stays on disk, and a recipe that runs that file by path then passes on a program no build makes.

    check_fresh_test_programs.py --build-dir DIR --targets FILE PROGRAM [PROGRAM ...]

- DIR is the build directory the build wrote to.
- FILE is the output of `ninja -C DIR -t targets all`; `-` reads standard input. Each line is `<output>: <rule>`,
  and the outputs are the paths the build declares, relative to DIR.
- PROGRAM is a path relative to DIR, spelled as the recipe runs it (`vm/luau/nevr_luau_memory_test.exe`).

A program is fresh when the build declares it and the file is on disk. Otherwise it is one of:
- stale: the file is on disk, and no output the build declares names it;
- missing: an output the build declares names it, and the file is not on disk;
- undeclared: no output the build declares names it, and the file is not on disk.

There is no age test. A build with nothing to do leaves an up-to-date program with the modification time of the link
that made it, so an age test would fail a correct rerun. The recipe removes each program before its build, so a
program on disk after the build is one the build made; this tool judges the listing and the disk.

Exit codes: 0 every program is fresh. 1 at least one is not: one FAIL line per problem on standard error, and every
program is still judged. 2 the arguments or the listing cannot be used, so nothing is judged.
"""

from __future__ import annotations

import argparse
import os
import pathlib
import re
import sys

PROG = "check_fresh_test_programs"
_OUTPUT_LINE = re.compile(r"^(?P<output>.+): (?P<rule>\S+)$")


class Refused(Exception):
    """An argument or the listing cannot be used, so no program is judged."""


def program_path(text: str) -> str:
    if os.path.isabs(text):
        raise Refused(f"{text}: a program is named relative to the build directory, not by an absolute path")
    rel = os.path.normpath(text)
    if rel in (".", os.pardir) or rel.startswith(os.pardir + os.sep):
        raise Refused(f"{text}: a program is named relative to the build directory and stays inside it")
    return rel


def declared_outputs(lines: list[str]) -> set[str]:
    outputs: set[str] = set()
    for number, raw in enumerate(lines, 1):
        line = raw.rstrip("\r\n")
        if not line.strip():
            continue
        match = _OUTPUT_LINE.match(line)
        if match is None:
            raise Refused(f"listing line {number} is not `<output>: <rule>`: {line!r}")
        outputs.add(os.path.normpath(match.group("output")))
    if not outputs:
        raise Refused("the listing declares no output, so no program can be judged")
    return outputs


def judge(build: pathlib.Path, declared: set[str], rel: str) -> tuple[str, str] | None:
    """None when the program is fresh, else (kind, why)."""
    declared_here = rel in declared
    on_disk = (build / rel).is_file()
    if declared_here and on_disk:
        return None
    if on_disk:
        return ("stale", "on disk, but no output the build declares names it: left by an earlier build, or its target "
                         "was removed or renamed")
    if declared_here:
        return ("missing", "the build declares it, and the build did not make the file")
    return ("undeclared", "no output the build declares names it, and it is not on disk")


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(prog=PROG, description=__doc__.split("\n", 1)[0])
    parser.add_argument("--build-dir", required=True, metavar="DIR", help="the build directory")
    parser.add_argument("--targets", required=True, metavar="FILE",
                        help="the output of `ninja -C DIR -t targets all`; `-` reads standard input")
    parser.add_argument("programs", nargs="+", metavar="PROGRAM", help="a program, relative to DIR")
    args = parser.parse_args(argv)

    build = pathlib.Path(args.build_dir)
    if not build.is_dir():
        print(f"{PROG}: FAIL build directory {args.build_dir} does not exist", file=sys.stderr)
        return 2
    label = build.name
    try:
        programs = [program_path(text) for text in args.programs]
        if args.targets == "-":
            lines = sys.stdin.read().splitlines()
        else:
            lines = pathlib.Path(args.targets).read_text(encoding="utf-8").splitlines()
        declared = declared_outputs(lines)
    except OSError as exc:
        print(f"{PROG}: FAIL cannot read listing {args.targets}: {exc.strerror}", file=sys.stderr)
        return 2
    except Refused as exc:
        print(f"{PROG}: FAIL {exc}", file=sys.stderr)
        return 2

    failures = 0
    for rel in programs:
        problem = judge(build, declared, rel)
        if problem is None:
            print(f"{PROG}: {label}: ok {rel}: declared by the build and on disk")
            continue
        failures += 1
        kind, why = problem
        print(f"{PROG}: FAIL {label}: {rel}: {kind}: {why}", file=sys.stderr)
    if failures:
        print(f"{PROG}: FAIL {label}: {failures} of {len(programs)} programs not fresh", file=sys.stderr)
        return 1
    print(f"{PROG}: {label}: {len(programs)} programs fresh")
    return 0


if __name__ == "__main__":
    sys.exit(main())
