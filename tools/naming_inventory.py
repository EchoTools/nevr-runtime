#!/usr/bin/env python3
"""Every name in the tree that breaks docs/standards/naming.md, with file:line (#131).

    python3 tools/naming_inventory.py                 # all categories, file:line, then a count table
    python3 tools/naming_inventory.py --summary       # counts only
    python3 tools/naming_inventory.py --category lifecycle-namespaces

It is the work list for the rename pass and the way to see what is left: a category that prints nothing is
done. It reads tracked and untracked-but-not-ignored files, and skips what test_naming.py skips
(src/legacy, extern, gen, docs/audits). The frozen `Nvr*` plugin/module ABI is not a finding (it is
FROZEN_NVR in tools/tests/test_naming.py). A name that is public (a config key, a log line, a DLL export,
a file name users touch) is listed here too, tagged `public`: it is not renamed without a decision, and the
rename PRs list each one as a deliberate exception.
"""
from __future__ import annotations

import argparse
import os
import pathlib
import re
import subprocess
import sys

REPO = pathlib.Path(os.environ.get("NEVR_NAMING_ROOT") or pathlib.Path(__file__).resolve().parents[1])
EXCLUDED_PREFIXES = ("src/legacy/", "extern/", "gen/", "docs/audits/", "build/")
SELF = frozenset({"docs/standards/naming.md", "tools/tests/test_naming.py", "tools/naming_inventory.py"})

CODE_EXT = (".h", ".hpp", ".cpp", ".cc", ".inc")
ANY_CASE = re.compile(r"(?<![A-Za-z])[Nn][Ee][Vv][Rr][A-Za-z0-9_]*")
CANONICAL = re.compile(r"(NEVR|Nevr|nevr)")
PASCAL_NAMESPACE = re.compile(r"^\s*namespace\s+([A-Z][A-Za-z0-9_]*(?:::[A-Za-z0-9_]+)*)\s*\{")
LIFECYCLE_NAMESPACE = re.compile(r"\bnamespace\s+((?:[Nn]evr(?:_runtime)?)::[Ll]ifecycle(?:::\w+)*)")
LOG_TAG = re.compile(r'"[^"\n]*\[([A-Z][A-Z0-9_]*(?:\.[A-Z0-9_]+)*)\]')
# Real spellings of "the project name" in a tag: [NEVR.AREA]. A bare [NEVR] and anything else is a finding.
ADD_LIBRARY = re.compile(r"^\s*add_(?:library|executable)\(\s*([A-Za-z0-9_]+)", re.M)
COMPILE_DEF = re.compile(r"target_compile_definitions\([^)]*\)", re.S)
EXPORT_DECL = re.compile(r"\bNEVR_MODULE_API\b[^;{(]*?\b([A-Za-z_][A-Za-z0-9_]*)\s*\(")


def project_files():
    out = subprocess.run(["git", "-C", str(REPO), "ls-files", "-z", "--cached", "--others", "--exclude-standard"],
                         check=True, capture_output=True, text=True).stdout
    for path in (p for p in out.split("\0") if p):
        if path.startswith(EXCLUDED_PREFIXES) or path in SELF:
            continue
        try:
            yield path, (REPO / path).read_text(encoding="utf-8")
        except (OSError, UnicodeDecodeError):
            continue


def lines_of(text):
    return enumerate(text.splitlines(), start=1)


def noncanonical_spellings(files):
    for path, text in files:
        for n, line in lines_of(text):
            for m in ANY_CASE.finditer(line):
                if not CANONICAL.match(m.group()):
                    yield path, n, m.group(), "a non-canonical spelling of the project name"


def lifecycle_namespaces(files):
    for path, text in files:
        if not path.endswith(CODE_EXT):
            continue
        for n, line in lines_of(text):
            m = LIFECYCLE_NAMESPACE.search(line)
            if m and not m.group(1).startswith("nevr::lifecycle"):
                yield path, n, m.group(1), "one lifecycle namespace spelled three ways; canonical nevr::lifecycle"


def pascal_namespaces(files):
    for path, text in files:
        if not path.endswith(CODE_EXT) or path.startswith("plugins/"):
            continue
        for n, line in lines_of(text):
            m = PASCAL_NAMESPACE.match(line)
            if m and not LIFECYCLE_NAMESPACE.search(line) and m.group(1) not in {"EchoVR", "NRadEngine"}:
                yield path, n, m.group(1), "namespace is nevr or nevr_<area> (EchoVR:: and NRadEngine:: are the game's)"


# Files that name the GAME's own log tags (to filter or match them), help text, and tests: not the project's tags.
GAME_TAG_FILES = ("src/runtime/log/builtin_filter.", "plugins/log-filter/", "src/runtime/hook/addresses.h",
                  "src/quest/sentinel/pinned_targets.h", "src/quest/social/social_abi.h", "src/runtime/log/symcache.",
                  "src/runtime/lifecycle/cli.cpp", "src/core/logging.cpp")


def log_tags(files):
    seen = set()
    for path, text in files:
        if not path.endswith(CODE_EXT) or path.startswith(GAME_TAG_FILES) or "/tests/" in path:
            continue
        for n, line in lines_of(text):
            for m in LOG_TAG.finditer(line):
                tag = m.group(1)
                if tag.startswith("NEVR.") or tag in {"INFO", "WARN", "ERROR", "DEBUG"}:
                    continue
                if (path, n, tag) in seen:
                    continue
                seen.add((path, n, tag))
                yield path, n, "[" + tag + "]", "public: a log line (score-log and scripts grep tags)"


def cmake_targets(files):
    for path, text in files:
        if not (path.endswith("CMakeLists.txt") or path.endswith(".cmake")):
            continue
        for m in ADD_LIBRARY.finditer(text):
            name = m.group(1)
            # Test executables and probes are not shipped surfaces; a name already in the family is fine.
            if name.startswith("nevr") or name.startswith("test_") or name.endswith(("_test", "_probe", "_probe_")):
                continue
            if name.endswith(("_test_hooks",)):
                continue
            n = text.count("\n", 0, m.start()) + 1
            yield path, n, name, "CMake target is nevr_<name> (an output file name stays: OUTPUT_NAME)"


def macros_without_prefix(files):
    for path, text in files:
        if not (path.endswith("CMakeLists.txt") or path.endswith(".cmake")):
            continue
        for m in COMPILE_DEF.finditer(text):
            for d in re.finditer(r"\b([A-Z][A-Z0-9_]+)(?==|\s|\))", m.group()):
                name = d.group(1)
                if name.startswith("NEVR_") or name in {"PRIVATE", "PUBLIC", "INTERFACE"} or name.startswith("TARGET"):
                    continue
                # System, toolchain and third-party macros are not the project's names.
                if name in {"NOMINMAX", "WIN32_LEAN_AND_MEAN", "PROTOBUF_STATIC_LIB", "CMAKE_BUILD_TYPE", "UNICODE",
                            "_UNICODE", "WIN32_WINNT", "_WIN32_WINNT"}:
                    continue
                n = text.count("\n", 0, m.start()) + 1
                yield path, n, name, "macro is NEVR_<NAME>"


def exports_without_prefix(files):
    for path, text in files:
        if not path.endswith(".h"):
            continue
        for n, line in lines_of(text):
            if "NEVR_MODULE_API" not in line or line.lstrip().startswith(("#", "//")):
                continue
            m = re.search(r"NEVR_MODULE_API\s+[^;(]*?\b([A-Za-z_][A-Za-z0-9_]*)\s*\(", line)
            if m and not m.group(1).startswith("NEVR_"):
                yield path, n, m.group(1), "public: a DLL export other DLLs may resolve by name; NEVR_<Verb><Noun>"


CATEGORIES = {
    "spellings": noncanonical_spellings,
    "lifecycle-namespaces": lifecycle_namespaces,
    "pascal-namespaces": pascal_namespaces,
    "log-tags": log_tags,
    "cmake-targets": cmake_targets,
    "macros": macros_without_prefix,
    "exports": exports_without_prefix,
}


def main(argv):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--category", choices=sorted(CATEGORIES))
    parser.add_argument("--summary", action="store_true")
    args = parser.parse_args(argv)
    files = list(project_files())
    names = [args.category] if args.category else list(CATEGORIES)
    totals = []
    for category in names:
        findings = sorted(set(CATEGORIES[category](files)))
        totals.append((category, len(findings), len({f[2] for f in findings})))
        if args.summary:
            continue
        print("== %s (%d)" % (category, len(findings)))
        for path, n, token, why in findings:
            print("%s:%d  %s  -- %s" % (path, n, token, why))
    print("== summary: category, findings, distinct names")
    for category, count, distinct in totals:
        print("%-22s %5d %5d" % (category, count, distinct))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
