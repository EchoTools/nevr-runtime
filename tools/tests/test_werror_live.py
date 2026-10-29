"""The MinGW build fails on every warning (#639), and the flag is live.

Two layers. The first reads the root CMakeLists.txt and always runs. The second takes the compile command
CMake recorded for a real runtime source, swaps the source for a fixture and compiles it with
-fsyntax-only, so the flags are the project's own: a deliberate `#warning` must fail the compile, and a clean
file must pass.
It skips when build/mingw-release has no compile_commands.json: it reads a build, it does not make one."""
from __future__ import annotations

import json
import pathlib
import re
import shlex
import subprocess
import unittest

REPO = pathlib.Path(__file__).resolve().parents[2]
FIXTURES = REPO / "tools" / "tests" / "fixtures" / "werror"
COMPILE_COMMANDS = REPO / "build" / "mingw-release" / "compile_commands.json"
# Built without the precompiled header (src/runtime/CMakeLists.txt), so its command has no -include to strip.
RUNTIME_SOURCE = "src/runtime/hook/export_tracer.cpp"


def strip_comments(text: str) -> str:
    return "\n".join(re.sub(r"(?<!\\)#.*", "", line) for line in text.splitlines())


def mingw_options(text: str) -> str:
    """The body of the add_compile_options() call inside the root `if(MINGW)` block."""
    code = strip_comments(text)
    block = re.search(r"\bif\s*\(\s*MINGW\s*\)\s*add_compile_options\s*\((.*?)\n\s*\)", code, re.S)
    if not block:
        raise AssertionError("CMakeLists.txt has no if(MINGW) add_compile_options(...) block")
    return block.group(1)


class WerrorFlagTest(unittest.TestCase):
    def test_the_mingw_options_carry_plain_werror(self):
        options = mingw_options((REPO / "CMakeLists.txt").read_text())
        self.assertIn("-Wall", options, "the block under test is empty or moved")
        self.assertRegex(options, re.compile(r"^\s*-Werror\s*$", re.M), "plain -Werror is missing from the MinGW compile options")

    def test_no_target_opts_out(self):
        # Tracked files only: an untracked tree inside the checkout (a worktree, a scratch copy) is not main.
        tracked = subprocess.run(["git", "ls-files", "--", "*CMakeLists.txt"], cwd=REPO, capture_output=True, text=True, check=True).stdout.split()
        self.assertTrue(tracked, "git ls-files lists no CMakeLists.txt: the scan would be watching nothing")
        offenders = []
        for rel in tracked:
            path = REPO / rel
            if rel.startswith(("build/", "extern/", "src/quest/")) or "/vcpkg" in rel:
                continue
            for number, line in enumerate(strip_comments(path.read_text(errors="replace")).splitlines(), 1):
                if "-Wno-error" in line:
                    offenders.append((rel, number, line.strip()))
        self.assertEqual(offenders, [], "a target opts out of -Werror")


def recorded_flags(argv: list, source: str) -> list:
    """The recorded argv without its source, outputs and dependency scanning. -fdeps-* only works with a -M
    option, which is removed with the dependency outputs, so GCC rejects the command if it stays (#713)."""
    kept = []
    skip = 0
    for arg in argv:
        if skip:
            skip -= 1
        elif arg in ("-o", "-MF", "-MT", "-MQ"):
            skip = 1
        elif arg in ("-c", "-MD", "-MMD") or arg == source or arg.startswith(("-fdeps-format=", "-fdeps-file=", "-fdeps-output=")):
            continue
        else:
            kept.append(arg)
    return kept


class RecordedFlagsTest(unittest.TestCase):
    """Needs no build tree: the filter that every live compile below depends on."""

    ARGV = ["g++", "-fmodules-ts", "-MD", "-MT", "x.obj", "-MF", "x.obj.d", "-fdeps-format=p1689r5",
            "-fdeps-file=x.ddi", "-fdeps-output=x.obj", "-Wall", "-Werror", "-o", "x.obj", "-c", "a.cpp"]

    def test_dependency_scanning_flags_are_removed_with_the_dependency_outputs(self):
        kept = recorded_flags(self.ARGV, "a.cpp")
        self.assertEqual(kept, ["g++", "-fmodules-ts", "-Wall", "-Werror"])


def compile_with(entry: dict, fixture: pathlib.Path) -> subprocess.CompletedProcess:
    """The recorded command with its source and outputs replaced: -fsyntax-only on the fixture."""
    kept = recorded_flags(shlex.split(entry["command"]), entry["file"])
    return subprocess.run(
        kept + ["-fsyntax-only", str(fixture)], cwd=entry["directory"], capture_output=True, text=True, timeout=120
    )


@unittest.skipUnless(COMPILE_COMMANDS.exists(), "no compile_commands.json in build/mingw-release")
class WerrorBuildTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        entries = json.loads(COMPILE_COMMANDS.read_text())
        cls.runtime = next(e for e in entries if e["file"].endswith(RUNTIME_SOURCE))

    def test_a_clean_file_compiles_with_the_project_flags(self):
        result = compile_with(self.runtime, FIXTURES / "clean.cpp")
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_a_warning_fails_the_compile(self):
        result = compile_with(self.runtime, FIXTURES / "warning_canary.cpp")
        self.assertNotEqual(result.returncode, 0, "a deliberate #warning compiled: -Werror is not live")
        self.assertIn("-Werror=cpp", result.stderr)
