#!/usr/bin/env python3
"""The BugSplat64.dll host units carry no test seam when NEVR_HOST_TESTING is not defined (docs/adr/0008).

src/runtime/host/ holds seams a test needs (a failing CreateEventW, a record builder that throws, an output
sink, a nested-fault probe). They exist only under NEVR_HOST_TESTING, which only the test-only library
nevr_host_core defines. This check looks at what is compiled, not at how the source is spelled:

  1. every unit of nevr_host_core is compiled twice with the build's own compiler and flags
     (compile_commands.json), with the define and without it, and the objects are read with nm:
       - no defined symbol of the object compiled without the define may say it is a seam (a seam of a shared
         unit that cannot sit behind the define is named in SHARED_UNIT_SEAMS, with the reason), and
       - every symbol that exists only with the define must say it is a seam (or be a standard-library
         template the seam instantiates), so a seam added under a name the pattern does not know is a failure;
  2. no compile command outside src/runtime/host/ and src/runtime/tests/ carries the define, and no host unit
     built for a target other than nevr_host_core does. This reads the given build directory and every
     sibling build directory that has a compile_commands.json (another preset or configuration). In the
     BUILD_TESTING=OFF configuration nevr_host_core is not created at all (it is inside if(BUILD_TESTING)), so
     there the define can only arrive through a CMake or flags line, which step 3 reads in every configuration;
  3. no tracked file spells the define in a way other than a gate line (`#if defined(NEVR_HOST_TESTING)` or
     `#ifdef NEVR_HOST_TESTING`; comments are removed from the line first, then the bare directive must match
     exactly, so `|| 1` between comments does not pass) or the one nevr_host_core
     target_compile_definitions line: #define or #cmakedefine (also in a .h.in), add_compile_definitions,
     target_compile_definitions elsewhere, add_compile_options, a -D in a script or a preset, and a gate that is
     always true (`|| 1`).

A seam defined inline in a header leaves no symbol for step 1; the host headers define their seams out of line
for that reason, and step 3 anchors every gate line.

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
SEAM_SYMBOL = re.compile(r"ForTest|Seam|Probe|FailWrites|g_fail_|g_emit_sink|g_record_pause|Claimed|claimed_"
                         r"|RuntimeRendezvous::Context\(|test", re.IGNORECASE)
# Seams that are named, by unit, because the unit is shared and cannot put them behind the define. A unit here is
# compiled into the host, into nevr_core, into the Quest sentinel and into test programs outside nevr_host_core;
# a gate on NEVR_HOST_TESTING would need the define on a src/core compile command, which step 2 forbids, and those
# test programs would lose the seam. Only the exact demangled name in the exact unit passes; any other seam-like
# symbol of the unit, and the same name in another unit, still fails.
SHARED_UNIT_SEAMS = {
    "crash_report.cpp": {
        "nevr_crash::ResetForTest()":
            "src/core/crash_report.cpp is the one crash report writer shared by the host, nevr_core, the Quest sentinel, "
            "test_crash_send and the Quest host test; the seam empties its log ring and written flag",
    },
}
# A symbol the define adds that is a standard-library template instantiation, not a seam (the throwing seam
# instantiates std::runtime_error's typeinfo).
LIBRARY_SYMBOL = re.compile(r"\bstd::|_ZT[ISV]St|_ZNSt|_ZSt|\.refptr\.")

C_SUFFIXES = (".h", ".hpp", ".hh", ".hxx", ".c", ".cc", ".cpp", ".cxx", ".inl", ".ipp", ".tpp")
C_TEMPLATE_SUFFIXES = tuple(suffix + ".in" for suffix in C_SUFFIXES)
# Files that name the define because they are this check; markdown only describes it.
EXEMPT_FILES = {"tools/verify_host_seams.py", "tools/tests/test_host_test_seams.py"}
EXEMPT_SUFFIXES = {".md"}
GATE_LINE = re.compile(rf"^\s*#\s*(?:if\s+defined\s*\(\s*{DEFINE}\s*\)|ifdef\s+{DEFINE})\s*$")
CMAKE_DEFINE_LINE = re.compile(rf"^\s*#\s*(?:cmakedefine01?|define)\b")
LAUNCHERS = {"ccache", "sccache", "distcc", "icecc"}
COMPILER_NAME = re.compile(r"(g\+\+|gcc|c\+\+|cc|clang\+\+|clang)(-[0-9.]+|-posix|-win32)?$")


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


def real_compiler_argv(argv):
    """The compile command without any compiler launcher (ccache and the like): the launcher is not the compiler."""
    at = 0
    while at < len(argv) and Path(argv[at]).name in LAUNCHERS:
        at += 1
    return argv[at:]


def nm_tool(compiler):
    """The nm that goes with a compiler. A launcher or any other program that is not a compiler is refused:
    running `nm` named after it would run the launcher itself (ccache -C clears the cache)."""
    path = Path(compiler)
    match = COMPILER_NAME.search(path.name)
    if path.name in LAUNCHERS or match is None:
        raise ValueError(f"{compiler} is not a compiler: cannot derive its nm")
    return str(path.with_name(path.name[:match.start()] + "nm"))


def seam_symbols(nm_output, unit=None):
    """Defined symbols (not 'U') in `nm -C` output whose name says it is a seam. `unit` is the file name of the
    compiled unit; a seam SHARED_UNIT_SEAMS names for that unit passes. A COMDAT section symbol (`.text$_ZN...`, `.pdata$`,
    `.xdata$`) is a section label that nm does not demangle: its mangled text can spell "test" inside other words
    (`...OffsetESt17...`), and the function it labels has its own demangled symbol, which is checked."""
    found = []
    named = SHARED_UNIT_SEAMS.get(unit, {})
    for line in nm_output.splitlines():
        parts = line.split(None, 2)
        if len(parts) < 3 or len(parts[1]) != 1 or parts[1] in "Uu":
            continue
        if parts[2].startswith(".") or parts[2].strip() in named:
            continue
        if SEAM_SYMBOL.search(parts[2]):
            found.append(line.strip())
    return found


def defined_symbols(nm_output):
    """{name: type} for the defined symbols (not 'U') in `nm -C` output."""
    symbols = {}
    for line in nm_output.splitlines():
        parts = line.split(None, 2)
        if len(parts) < 3 or len(parts[1]) != 1 or parts[1] in "Uu":
            continue
        symbols[parts[2].strip()] = parts[1]
    return symbols


def define_only_unnamed(with_define, without_define):
    """Symbols that exist only in the object compiled with the define and do not say they are seams."""
    return sorted(name for name in with_define
                  if name not in without_define and not SEAM_SYMBOL.search(name) and not LIBRARY_SYMBOL.search(name))


def compile_unit(entry, scratch, with_define):
    """Compile one nevr_host_core unit, with or without the define; returns (object path, flags removed, compiler)."""
    argv = real_compiler_argv(shlex.split(entry["command"]))
    removed = 0
    if not with_define:
        argv, removed = strip_define(argv)
    obj = scratch / (Path(entry["file"]).name + (".with.obj" if with_define else ".obj"))
    if "-o" in argv:
        argv[argv.index("-o") + 1] = str(obj)
    else:
        argv += ["-o", str(obj)]
    argv = [a for a in argv if not a.startswith("-fdiagnostics-color")]
    result = subprocess.run(argv, cwd=entry["directory"], capture_output=True, text=True)
    if result.returncode != 0:
        raise RuntimeError(f"{Path(entry['file']).name} did not compile {'with' if with_define else 'without'} "
                           f"{DEFINE} (rc={result.returncode}):\n{result.stderr}")
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


def without_comments(line):
    """The line with its block comments (each replaced by a space) and its trailing // comment removed. A /* that is
    not closed on the line stays, so a gate line that continues into a comment does not match."""
    return re.sub(r"//.*", "", re.sub(r"/\*.*?\*/", " ", line))


def is_c_file(path):
    return path.endswith(C_SUFFIXES) or path.endswith(C_TEMPLATE_SUFFIXES)


def stray_definitions(files):
    """(path, line number, text) for every line of the given {path: text} that spells the define in a way
    that is not an anchored gate line, a comment, or the one allowed target_compile_definitions line."""
    offenders = []
    for path, text in sorted(files.items()):
        for number, line in enumerate(text.splitlines(), 1):
            if DEFINE not in line:
                continue
            code = line.strip()
            if is_c_file(path):
                if code.startswith("//") or code.startswith("*") or code.startswith("/*"):
                    continue
                if GATE_LINE.match(without_comments(line)):
                    continue
            elif code.startswith("#") and not CMAKE_DEFINE_LINE.match(line):  # a cmake / shell / python comment
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
        if not name or name in EXEMPT_FILES or Path(name).suffix in EXEMPT_SUFFIXES:
            continue
        path = REPO / name
        if not path.is_file():
            continue
        try:
            files[name] = path.read_text(encoding="utf-8")
        except (UnicodeDecodeError, OSError):
            continue  # a binary file spells nothing
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
            obj, removed, compiler = compile_unit(entry, scratch, with_define=False)
            obj_with, _, _ = compile_unit(entry, scratch, with_define=True)
        except RuntimeError as error:
            failures.append(str(error))
            continue
        if removed != 1:
            failures.append(f"{name}: expected exactly one -D{DEFINE} in the build's flags, found {removed}")
        try:
            nm = nm_tool(compiler)
        except ValueError as error:
            failures.append(f"{name}: {error}")
            continue
        listings = {}
        for label, path in (("without", obj), ("with", obj_with)):
            listing = subprocess.run([nm, "-C", str(path)], capture_output=True, text=True)
            if listing.returncode != 0 or not listing.stdout.strip():
                failures.append(f"{name}: nm could not read {path} (rc={listing.returncode}): {listing.stderr}")
            listings[label] = listing.stdout
        if len(listings) != 2 or not all(listings.values()):
            continue
        seams = seam_symbols(listings["without"], unit=name)
        unnamed = define_only_unnamed(defined_symbols(listings["with"]), defined_symbols(listings["without"]))
        print(f"verify_host_seams: {name}: compiled without -D{DEFINE}, {len(seams)} seam-like symbol(s); "
              f"{len(unnamed)} define-only symbol(s) not named as a seam")
        for symbol in seams:
            failures.append(f"{name}: seam symbol in the object compiled without the define: {symbol}")
        for symbol in unnamed:
            failures.append(f"{name}: a symbol that exists only with the define does not say it is a seam: {symbol}")
        obj.unlink()
        obj_with.unlink()

    failures += [f"compile command: {o}" for o in stray_define_commands(commands)]
    others = [other for other in sorted(build.parent.glob("*/compile_commands.json")) if other != database]
    for other in others:
        failures += [f"compile command ({other.parent.name}): {o}"
                     for o in stray_define_commands(json.loads(other.read_text(encoding="utf-8")))]
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
    print(f"verify_host_seams: OK ({len(units)} units, no seam symbol, no stray {DEFINE}, "
          f"{len(others)} other build configuration(s) checked)")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
