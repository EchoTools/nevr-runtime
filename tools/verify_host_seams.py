#!/usr/bin/env python3
"""The BugSplat64.dll host units carry no test seam when NEVR_HOST_TESTING is not defined (docs/adr/0008).

src/runtime/host/ holds seams a test needs (a failing CreateEventW, a throwing record builder, an output
sink, a nested-fault probe). They exist only under NEVR_HOST_TESTING, which only the test-only library
nevr_host_core defines. This check looks at what is compiled, not at how the source is spelled:

  1. every unit of nevr_host_core is compiled again with the build's own compiler and flags
     (compile_commands.json) minus the define, and the object is read with nm: no defined symbol may
     say it is a seam;
  2. no compile command outside src/runtime/host/ and src/runtime/tests/ carries the define, and no
     host unit built for a target other than nevr_host_core carries it;
  3. no tracked file defines the name by any spelling (#define, add_compile_definitions,
     target_compile_definitions, add_compile_options, a -D in flags or a preset) other than the one
     target_compile_definitions line for nevr_host_core.

usage: verify_host_seams.py <build-dir>      (the directory holding compile_commands.json)
exit 0: clean. exit 1: a seam, a stray define or an unreadable subject (never a silent pass).
"""

import json
import re
import shlex
import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
DEFINE = "NEVR_HOST_TESTING"
HOST_CORE_DIR = "nevr_host_core.dir/"
HOST_SOURCES = "src/runtime/host/"
TEST_SOURCES = "src/runtime/tests/"
ALLOWED_DEFINITION = ("src/runtime/CMakeLists.txt",
                      re.compile(r"^\s*target_compile_definitions\(\s*nevr_host_core\s+PUBLIC\s+NEVR_HOST_TESTING\s*\)\s*$"))

# A defined symbol whose demangled name says it is a seam.
SEAM_SYMBOL = re.compile(r"ForTest|Seam|Probe|FailWrites|g_fail_|g_emit_sink|g_record_pause|Claimed|claimed_|test",
                         re.IGNORECASE)

TEXT_SUFFIXES = {".h", ".hpp", ".c", ".cc", ".cpp", ".cmake", ".txt", ".json", ".sh", ".yml", ".yaml", ".in",
                 ".toml", ".mk", ".env", ".just", ""}
SKIP_SUFFIXES = {".md", ".py"}


def strip_define(argv):
    """The compile arguments minus every spelling of -DNEVR_HOST_TESTING; returns (argv, removed count)."""
    out, removed, skip_next = [], 0, False
    for index, arg in enumerate(argv):
        if skip_next:
            skip_next = False
            continue
        if arg == "-D" and index + 1 < len(argv) and argv[index + 1].split("=")[0] == DEFINE:
            removed += 1
            skip_next = True
            continue
        if arg.startswith("-D") and arg[2:].split("=")[0] == DEFINE:
            removed += 1
            continue
        out.append(arg)
    return out, removed


def nm_tool(compiler):
    path = Path(compiler)
    name = path.name.replace("g++", "nm").replace("gcc", "nm").replace("c++", "nm")
    return str(path.with_name(name))


def seam_symbols(nm_output):
    """Defined symbols (not 'U') in `nm -C` output whose name says it is a seam."""
    found = []
    for line in nm_output.splitlines():
        parts = line.split(None, 2)
        if len(parts) < 3 or len(parts[1]) != 1 or parts[1] in "Uu":
            continue
        if SEAM_SYMBOL.search(parts[2]):
            found.append(line.strip())
    return found


def compile_without_define(entry, scratch):
    """Compile one nevr_host_core unit without the define; returns (object path, flags removed)."""
    argv = shlex.split(entry["command"])
    argv, removed = strip_define(argv)
    obj = scratch / (Path(entry["file"]).name + ".obj")
    if "-o" in argv:
        argv[argv.index("-o") + 1] = str(obj)
    else:
        argv += ["-o", str(obj)]
    argv = [a for a in argv if not a.startswith("-fdiagnostics-color")]
    result = subprocess.run(argv, cwd=entry["directory"], capture_output=True, text=True)
    if result.returncode != 0:
        raise RuntimeError(f"{Path(entry['file']).name} did not compile without {DEFINE} "
                           f"(rc={result.returncode}):\n{result.stderr}")
    return obj, removed, argv[0]


def relative(entry):
    try:
        return str(Path(entry["file"]).resolve().relative_to(REPO))
    except ValueError:
        return entry["file"]


def stray_define_commands(commands):
    """Compile commands that carry the define where they must not."""
    offenders = []
    for entry in commands:
        text = entry.get("command") or " ".join(entry.get("arguments", []))
        if DEFINE not in text:
            continue
        source = relative(entry)
        output = entry.get("output", "")
        if HOST_CORE_DIR in output:
            continue  # the test-only library itself: its units are compiled with the define
        if source.startswith(HOST_SOURCES):
            offenders.append(f"{source}: built for {output} with -D{DEFINE}")
        elif not source.startswith(TEST_SOURCES):
            offenders.append(f"{source}: compiled with -D{DEFINE}")
    return offenders


def stray_definitions(files):
    """(path, line number, text) for every line of the given {path: text} that spells the define in a way
    that is not a #if / #ifdef test of it, a comment, or the one allowed target_compile_definitions line."""
    offenders = []
    test_of_define = re.compile(rf"^\s*#\s*(if|elif)\b.*\bdefined\s*\(\s*{DEFINE}\s*\)|^\s*#\s*ifn?def\s+{DEFINE}\b")
    for path, text in sorted(files.items()):
        suffix = Path(path).suffix
        for number, line in enumerate(text.splitlines(), 1):
            if DEFINE not in line:
                continue
            code = line.strip()
            if suffix in (".h", ".hpp", ".c", ".cc", ".cpp"):
                if code.startswith("//") or code.startswith("*") or code.startswith("/*"):
                    continue
                if test_of_define.match(line):
                    continue
            elif code.startswith("#"):  # a cmake / shell comment
                continue
            allowed_path, allowed_line = ALLOWED_DEFINITION
            if path == allowed_path and allowed_line.match(line):
                continue
            offenders.append((path, number, code))
    return offenders


def tracked_text_files():
    names = subprocess.run(["git", "-C", str(REPO), "ls-files"], capture_output=True, text=True,
                           check=True).stdout.split("\n")
    files = {}
    for name in names:
        suffix = Path(name).suffix
        if not name or suffix in SKIP_SUFFIXES or suffix not in TEXT_SUFFIXES:
            continue
        path = REPO / name
        if not path.is_file():
            continue
        try:
            files[name] = path.read_text(encoding="utf-8")
        except (UnicodeDecodeError, OSError):
            continue
    return files


def main(argv):
    if len(argv) != 2:
        print(__doc__)
        return 1
    build = Path(argv[1]).resolve()
    database = build / "compile_commands.json"
    if not database.is_file():
        print(f"verify_host_seams: FAIL: {database} not found (configure the build first)")
        return 1
    commands = json.loads(database.read_text(encoding="utf-8"))
    units = [e for e in commands if HOST_CORE_DIR in e.get("output", "")]
    if not units:
        print("verify_host_seams: FAIL: no nevr_host_core unit in compile_commands.json (a check watching nothing)")
        return 1

    failures = []
    scratch = build / "host-seams-check"
    scratch.mkdir(exist_ok=True)
    for entry in units:
        name = Path(entry["file"]).name
        try:
            obj, removed, compiler = compile_without_define(entry, scratch)
        except RuntimeError as error:
            failures.append(str(error))
            continue
        if removed != 1:
            failures.append(f"{name}: expected exactly one -D{DEFINE} in the build's flags, found {removed}")
        listing = subprocess.run([nm_tool(compiler), "-C", str(obj)], capture_output=True, text=True)
        if listing.returncode != 0 or not listing.stdout.strip():
            failures.append(f"{name}: nm could not read {obj} (rc={listing.returncode}): {listing.stderr}")
            continue
        seams = seam_symbols(listing.stdout)
        print(f"verify_host_seams: {name}: compiled without -D{DEFINE}, {len(seams)} seam-like symbol(s)")
        for symbol in seams:
            failures.append(f"{name}: seam symbol in the object compiled without the define: {symbol}")
        obj.unlink()

    failures += [f"compile command: {o}" for o in stray_define_commands(commands)]
    failures += [f"{p}:{n}: defines {DEFINE} outside the nevr_host_core line: {t}"
                 for p, n, t in stray_definitions(tracked_text_files())]
    try:
        scratch.rmdir()
    except OSError:
        pass
    if failures:
        for failure in failures:
            print(f"verify_host_seams: FAIL: {failure}")
        return 1
    print(f"verify_host_seams: OK ({len(units)} units, no seam symbol, no stray {DEFINE})")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
