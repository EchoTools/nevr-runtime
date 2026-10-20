"""The MinGW build fails on every warning (#639), and the flag is live.

Two layers. The first reads the root CMakeLists.txt and always runs. The second takes the compile command
CMake recorded for a real runtime source, swaps the source for a fixture and compiles it with
-fsyntax-only, so the flags are the project's own: a deliberate `#warning` must fail the compile, a clean
file must pass, and the frozen legacy target (src/legacy/gamepatches) must still pass the same warning.
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
LEGACY_SOURCE = "src/legacy/gamepatches/patches.cpp"


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

    def test_only_the_frozen_legacy_target_opts_out(self):
        offenders = []
        for path in REPO.rglob("CMakeLists.txt"):
            rel = path.relative_to(REPO).as_posix()
            if rel.startswith(("build/", "extern/", "src/quest/")) or "/vcpkg" in rel:
                continue
            for number, line in enumerate(strip_comments(path.read_text(errors="replace")).splitlines(), 1):
                if "-Wno-error" in line:
                    offenders.append((rel, number, line.strip()))
        self.assertEqual(len(offenders), 1, f"-Wno-error appears outside the legacy exemption: {offenders}")
        self.assertIn("gamepatcheslegacy", offenders[0][2])


def compile_with(entry: dict, fixture: pathlib.Path) -> subprocess.CompletedProcess:
    """The recorded command with its source and outputs replaced: -fsyntax-only on the fixture."""
    argv = shlex.split(entry["command"])
    kept = []
    skip = 0
    for index, arg in enumerate(argv):
        if skip:
            skip -= 1
        elif arg in ("-o", "-MF", "-MT", "-MQ"):
            skip = 1
        elif arg in ("-c", "-MD", "-MMD") or arg == entry["file"]:
            continue
        else:
            kept.append(arg)
    return subprocess.run(
        kept + ["-fsyntax-only", str(fixture)], cwd=entry["directory"], capture_output=True, text=True, timeout=120
    )


@unittest.skipUnless(COMPILE_COMMANDS.exists(), "no compile_commands.json in build/mingw-release")
class WerrorBuildTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        entries = json.loads(COMPILE_COMMANDS.read_text())
        cls.runtime = next(e for e in entries if e["file"].endswith(RUNTIME_SOURCE))
        cls.legacy = next(e for e in entries if e["file"].endswith(LEGACY_SOURCE))

    def test_a_clean_file_compiles_with_the_project_flags(self):
        result = compile_with(self.runtime, FIXTURES / "clean.cpp")
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_a_warning_fails_the_compile(self):
        result = compile_with(self.runtime, FIXTURES / "warning_canary.cpp")
        self.assertNotEqual(result.returncode, 0, "a deliberate #warning compiled: -Werror is not live")
        self.assertIn("-Werror=cpp", result.stderr)

    def test_the_frozen_legacy_target_still_compiles_a_warning(self):
        result = compile_with(self.legacy, FIXTURES / "warning_canary.cpp")
        self.assertEqual(result.returncode, 0, result.stderr)
