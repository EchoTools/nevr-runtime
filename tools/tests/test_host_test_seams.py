"""The host units' test seams cannot ship (docs/adr/0008).

src/runtime/host/ carries seams a test needs (a CreateEventW that fails, a record builder that
throws, an output sink). They exist only under NEVR_HOST_TESTING, which only the test-only library
nevr_host_core defines. The target that builds the host into BugSplat64.dll must not define it, so
the seams are not in the shipped file.
"""

import re
import subprocess
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
HOST = REPO / "src" / "runtime" / "host"
DEFINE = "NEVR_HOST_TESTING"
SEAM_NAMES = (
    "TestSeams", "Seams()", "SetEmitLoadFailureForTest", "g_fail_emit_for_test", "SetEmitSinkForTest",
    "g_emit_sink", "ResetActiveForTest", "SetInstallEventForTest", "claimed_", "JournalClaimed",
    "EmitSink", "Context() const",
)


def code_lines(path):
    """(line number, text) for the lines that are code: block and line comments dropped."""
    text = path.read_text(encoding="utf-8")
    text = re.sub(r"/\*.*?\*/", lambda m: "\n" * m.group(0).count("\n"), text, flags=re.S)
    for number, line in enumerate(text.splitlines(), 1):
        yield number, re.sub(r"//.*", "", line)


def lines_outside_testing_blocks(path):
    """Code lines of `path` that are not inside an `#if defined(NEVR_HOST_TESTING)` branch."""
    stack = []  # one entry per open conditional: True when this branch is the testing one
    for number, line in code_lines(path):
        directive = re.match(r"\s*#\s*(if|ifdef|ifndef|elif|else|endif)\b(.*)", line)
        if directive:
            kind, rest = directive.groups()
            if kind in ("if", "ifdef", "ifndef"):
                stack.append(kind != "ifndef" and DEFINE in rest)
            elif kind == "elif":
                stack[-1] = DEFINE in rest
            elif kind == "else":
                stack[-1] = False
            elif kind == "endif":
                stack.pop()
            continue
        if not any(stack):
            yield number, line


class HostTestSeamsTest(unittest.TestCase):
    def test_every_seam_is_inside_a_testing_block(self):
        offenders = []
        for path in sorted(HOST.glob("*.[ch]*")):
            for number, line in lines_outside_testing_blocks(path):
                for name in SEAM_NAMES:
                    if name in line:
                        offenders.append(f"{path.name}:{number}: {name}: {line.strip()}")
        self.assertEqual(offenders, [], "a test seam is compiled without NEVR_HOST_TESTING")

    def test_the_helpers_read_the_seams_only_under_the_define(self):
        text = (HOST / "runtime_rendezvous.cpp").read_text(encoding="utf-8")
        for helper in ("FailEventCreate", "FailHookBytesRead", "InstallWaitMs"):
            body = re.search(rf"\n(?:bool|DWORD) {helper}\(\) \{{(.*?)\n\}}", text, re.S).group(1)
            self.assertTrue(re.search(rf"#if defined\({DEFINE}\).*Seams\(\).*#else.*#endif", body, re.S), helper)

    def test_only_the_test_library_defines_it(self):
        files = subprocess.run(["git", "-C", str(REPO), "grep", "-l", DEFINE], capture_output=True,
                               text=True).stdout.split()
        allowed_sources = {
            p for p in files
            if p.startswith("src/runtime/host/") or p.startswith("src/runtime/tests/")
            or p == "tools/tests/test_host_test_seams.py"
        }
        others = sorted(set(files) - allowed_sources)
        self.assertEqual(others, ["src/runtime/CMakeLists.txt"], "who else mentions NEVR_HOST_TESTING")
        cmake = (REPO / "src" / "runtime" / "CMakeLists.txt").read_text(encoding="utf-8")
        defining = [m.start() for m in re.finditer(rf"target_compile_definitions\(([A-Za-z0-9_]+)[^)]*{DEFINE}", cmake)]
        self.assertEqual(len(defining), 1)
        target = re.search(rf"target_compile_definitions\(([A-Za-z0-9_]+)[^)]*{DEFINE}", cmake).group(1)
        self.assertEqual(target, "nevr_host_core")
        # ... and that library is created inside the BUILD_TESTING block.
        before = cmake[:cmake.index("add_library(nevr_host_core")]
        self.assertGreater(before.rfind("if(BUILD_TESTING)"), before.rfind("endif()"))


if __name__ == "__main__":
    unittest.main()
