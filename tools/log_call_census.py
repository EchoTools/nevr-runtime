#!/usr/bin/env python3
"""Recount the call-site figures in docs/adr/0027-structured-logging.md ("Measured").

Reads the tree from git only (git ls-tree and git show at one revision): no checkout, no build.
Each figure is a regex count over the C and C++ sources of that revision, and prints under the
name the ADR's table uses. Comment lines and definitions (a `void Log(` header) are not counted. src/legacy/ is excluded (frozen, ADR 0027 Measured).

Usage:
  python3 tools/log_call_census.py [REV]     # REV defaults to HEAD

The ADR's figures were taken at f41d66b4e0de7e5430bda7a133491be23dad5417; run this at that
revision to reproduce them.
"""

import collections
import re
import subprocess
import sys

SOURCE_EXTENSIONS = (".cpp", ".cc", ".c", ".h", ".hpp")
SCOPE_ROOTS = ("src/", "plugins/")

# A printf-style call: `Log(` not reached through a member, a scope or an identifier tail.
PRINTF_LOG = re.compile(r"(?<![A-Za-z0-9_.>:])Log\(")
LEVEL = re.compile(r"\s*EchoVR::LogLevel::(Debug|Info|Warning|Error)\b")
DEFINITION_PREFIX = re.compile(r"\b(void|bool|int|VOID|BOOL|CHAR|extern|static|inline|auto|explicit)\s*(\w+::)*$")


def git(*args):
    return subprocess.run(["git", *args], check=True, capture_output=True, text=True).stdout


def sources(rev):
    """Yield (path, text) for every C or C++ source under the scope roots at rev."""
    names = git("ls-tree", "-r", "--name-only", rev, "--", *SCOPE_ROOTS).splitlines()
    for name in names:
        if name.endswith(SOURCE_EXTENSIONS):
            yield name, git("show", f"{rev}:{name}")


def is_comment_or_definition(text, start):
    line_start = text.rfind("\n", 0, start) + 1
    prefix = text[line_start:start]
    stripped = prefix.lstrip()
    if stripped.startswith(("//", "*", "/*")) or "//" in prefix:
        return True
    return DEFINITION_PREFIX.search(prefix) is not None


def in_quest(path):
    return path.startswith("src/quest/")


def in_legacy(path):
    return path.startswith("src/legacy/")


def in_tests(path):
    return path.startswith("src/runtime/tests/") or "/tests/" in path


def in_session_router(path):
    return path == "src/runtime/compat/session_router.cpp"


def count_regex(rev, pattern, scope):
    rx = re.compile(pattern)
    total = 0
    for path, text in sources(rev):
        if not scope(path):
            continue
        for m in rx.finditer(text):
            if not is_comment_or_definition(text, m.start()):
                total += 1
    return total


def main():
    rev = sys.argv[1] if len(sys.argv) > 1 else "HEAD"
    rev = git("rev-parse", "--verify", rev).strip()
    print(f"revision {rev}")

    def printf_scope(path):
        return path.startswith(SCOPE_ROOTS) and not in_quest(path) and not in_legacy(path) and not in_tests(path) and not in_session_router(path)

    levels = collections.Counter()
    per_dir = collections.Counter()
    files = set()
    for path, text in sources(rev):
        if not printf_scope(path):
            continue
        for m in PRINTF_LOG.finditer(text):
            if is_comment_or_definition(text, m.start()):
                continue
            level = LEVEL.match(text, m.end())
            levels[level.group(1) if level else "expression"] += 1
            files.add(path)
            per_dir[path.split("/")[0] + "/" + path.split("/")[1]] += 1

    total = sum(levels.values())
    print(f"printf Log( calls outside src/quest/, src/runtime/tests/ and session_router.cpp: {total} in {len(files)} files")
    for key in sorted(per_dir):
        print(f"  {key} {per_dir[key]}")
    for key in ("Info", "Warning", "Debug", "Error", "expression"):
        print(f"  level {key} {levels[key]}")

    print("other writers (outside src/quest/ unless noted):")
    print(f"  TeeFprintf {count_regex(rev, r'TeeFprintf\(', printf_scope)}")
    print(f"  BlfLog {count_regex(rev, r'(?<![A-Za-z0-9_])BlfLog\(', printf_scope)}")
    print(f"  ServerFatal {count_regex(rev, r'(?<![A-Za-z0-9_])ServerFatal\(', printf_scope)}")
    print(f"  FatalError {count_regex(rev, r'(?<![A-Za-z0-9_])FatalError\(', printf_scope)}")
    print(f"  VehPrintf {count_regex(rev, r'(?<![A-Za-z0-9_])VehPrintf\(', lambda p: p.startswith(SCOPE_ROOTS))}")
    print(f"  fprintf/vfprintf to stderr {count_regex(rev, r'v?fprintf\(\s*stderr', lambda p: p.startswith(SCOPE_ROOTS) and not in_quest(p))}")
    print(f"  WriteLog direct calls {count_regex(rev, r'EchoVR::WriteLog\(', printf_scope)}")
    print(f"  Router::Log definitions/calls in session_router.cpp {count_regex(rev, r'(?<![A-Za-z0-9_.>:])Log\(', in_session_router)}")
    print(f"  what() passed to a log line {count_regex(rev, r'Log\([^;]*\bwhat\(\)', printf_scope)}")

    print("plugins:")
    print(f"  PluginLog* calls {count_regex(rev, r'(?<![A-Za-z0-9_])PluginLog(Warning|Error|Debug|Fields)?\(', lambda p: p.startswith(SCOPE_ROOTS) and not in_tests(p))}")

    print("quest:")
    print(f"  LogFields/LogFieldList calls (LogField(...) field constructors not counted) {count_regex(rev, r'(?<![A-Za-z0-9_])(LogFields|LogFieldList)\(', lambda p: in_quest(p) and not in_tests(p))}")
    print(f"  __android_log_* calls {count_regex(rev, r'__android_log_(print|write|vprint)\(', lambda p: in_quest(p) and not in_tests(p))}")

    print("tests:")
    print(f"  text-search helper uses {count_regex(rev, r'(TestLogContains(AtLevel)?|LogLinesContaining|WaitForLogLines|LogContains)\(', in_tests)}")


if __name__ == "__main__":
    main()
