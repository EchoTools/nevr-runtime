"""ADR 0041 for the test fixture DLLs and the VM probe (#2051): CMake compiles them, the scripts do not.

Three things are held here:
  1. the scripts and recipes this issue converted contain no compiler or linker command (ADR 0041's list);
  2. the DLL stems tools/gen_plugin_signature_fixtures.py expects are exactly the targets
     src/runtime/tests/sigfix/CMakeLists.txt builds, and the probe systest.py copies is the file
     tools/winvm/CMakeLists.txt builds, at the path systest.py reads;
  3. the scripts behave on the built files: a missing file is named, the probe is copied, not compiled.
"""

import contextlib
import io
import pathlib
import re
import sys
import tempfile
import unittest
from unittest import mock

REPO = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "tools"))
sys.path.insert(0, str(REPO / "tools" / "winvm"))
import gen_plugin_signature_fixtures as sigfix  # noqa: E402
import systest  # noqa: E402

SIGFIX_CMAKE = REPO / "src/runtime/tests/sigfix/CMakeLists.txt"
CAP_LIST = REPO / "src/runtime/tests/sigfix/cap_list.h"
WINVM_CMAKE = REPO / "tools/winvm/CMakeLists.txt"

# ADR 0041, "Finding a violation", V1: a compiler or linker command, in POSIX ERE there, in Python here.
COMPILER = re.compile(
    r"(^|[^A-Za-z0-9_.+-])(g\+\+|gcc|clang\+\+|clang|ndk-build|windres|x86_64-w64-mingw32-(gcc|g\+\+|ld|ar|windres)"
    r"|aarch64-linux-android[0-9]*-clang(\+\+)?)($|[^A-Za-z0-9_+-])")


def recipe_body(justfile: str, name: str) -> str:
    """The text of recipe `name`: its header line through the last indented line."""
    match = re.search(rf"^{re.escape(name)}(?=[ :])[^\n]*:[^\n]*\n((?:[ \t]+[^\n]*\n|\n)*)", justfile, re.M)
    assert match, f"justfile has no recipe {name}"
    return match.group(0)


def compiler_lines(text: str) -> list[str]:
    return [line for line in text.splitlines() if COMPILER.search(line)]


class NoCompilerCommandTest(unittest.TestCase):
    def test_the_fixture_script_and_the_vm_scripts_run_no_compiler(self):
        for path in ("tools/gen_plugin_signature_fixtures.py", "tools/winvm/systest.py", "tools/winvm/README.md"):
            with self.subTest(path=path):
                self.assertEqual(compiler_lines((REPO / path).read_text()), [], f"{path} names a compiler (ADR 0041)")

    def test_the_recipes_that_run_them_run_no_compiler(self):
        justfile = (REPO / "justfile").read_text()
        for name in ("test-auth-unit", "test-winvm"):
            with self.subTest(recipe=name):
                self.assertEqual(compiler_lines(recipe_body(justfile, name)), [], f"recipe {name} names a compiler")

    def test_the_pattern_finds_what_it_is_meant_to_find(self):
        for line in ('x86_64-w64-mingw32-gcc -shared -o a.dll a.c', '    g++ -std=c++17 a.cpp', 'cc = "x86_64-w64-mingw32-gcc"',
                     'subprocess.run(["clang++", "-c"])', 'windres a.rc'):
            self.assertTrue(COMPILER.search(line), line)
        for line in ("mingw-w64-gcc package", "cmake --build --preset mingw-release --target gai_probe", "# ggcc"):
            self.assertFalse(COMPILER.search(line), line)

    def test_the_recipe_builds_the_dlls_with_cmake_before_the_script_reads_them(self):
        body = recipe_body((REPO / "justfile").read_text(), "test-auth-unit")
        build = body.index("--target sigfix_dlls")
        script = body.index("tools/gen_plugin_signature_fixtures.py")
        self.assertLess(build, script)
        self.assertIn("--dll-dir", body[script:])


class TargetsMatchTheScriptTest(unittest.TestCase):
    def cmake_stems(self) -> set[str]:
        text = SIGFIX_CMAKE.read_text()
        stems = {m.group(1) for m in re.finditer(r"^sigfix_dll\((\w+) ", text, re.M)}
        count = int(re.search(r"set\(SIGFIX_CAP_COUNT (\d+)\)", text).group(1))
        self.assertIn("capdep_${padded}", text)
        return {s for s in stems} | {f"capdep_{i:03d}" for i in range(1, count + 1)}

    def test_the_script_expects_exactly_the_dlls_cmake_builds(self):
        stems = self.cmake_stems() - {"capdep_${padded}"}
        self.assertEqual(set(sigfix.BUILT_DLLS), stems)
        self.assertEqual(len(sigfix.BUILT_DLLS), len(set(sigfix.BUILT_DLLS)))

    def test_the_cap_count_agrees_across_cmake_the_header_and_the_script(self):
        count = int(re.search(r"set\(SIGFIX_CAP_COUNT (\d+)\)", SIGFIX_CMAKE.read_text()).group(1))
        listed = [int(n) for n in re.findall(r"X\((\d+)\)", CAP_LIST.read_text())]
        self.assertEqual(listed, list(range(1, count + 1)))
        self.assertEqual(sigfix.CAP_COUNT, count)

    def test_the_two_cap_importers_name_64_and_65_libraries(self):
        text = SIGFIX_CMAKE.read_text()
        self.assertRegex(text, r"sigfix_dll\(cap64 .*CAP_USED=64")
        self.assertRegex(text, r"sigfix_dll\(cap65 .*CAP_USED=65")

    def test_every_dll_is_written_where_the_recipe_reads_it(self):
        text = SIGFIX_CMAKE.read_text()
        self.assertIn('set(SIGFIX_OUTPUT_DIR "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/sigfix")', text)
        self.assertIn("-nostdlib", text)
        self.assertIn("-Wl,--entry=DllMain", text)


class FixtureScriptTest(unittest.TestCase):
    def test_a_missing_dll_is_named_with_the_target_that_builds_it(self):
        with tempfile.TemporaryDirectory(dir="/var/tmp") as built, tempfile.TemporaryDirectory(dir="/var/tmp") as work:
            (pathlib.Path(built) / "plain.dll").write_bytes(b"MZ")
            with self.assertRaises(SystemExit) as raised:
                sigfix.collect_built_dlls(pathlib.Path(work), built)
        message = str(raised.exception)
        self.assertIn("plain2.dll", message)
        self.assertIn("capdep_065.dll", message)
        self.assertNotIn("plain.dll,", message)
        self.assertIn("--target sigfix_dlls", message)

    def test_the_script_requires_the_directory_cmake_wrote(self):
        err = io.StringIO()
        with contextlib.redirect_stderr(err), mock.patch.object(sys, "argv", ["gen", "out"]), \
                self.assertRaises(SystemExit) as raised:
            sigfix.main()
        self.assertEqual(raised.exception.code, 2)
        self.assertIn("--dll-dir", err.getvalue())


class ProbeTest(unittest.TestCase):
    def test_systest_reads_the_file_the_probe_target_writes(self):
        cmake = WINVM_CMAKE.read_text()
        self.assertIn("add_executable(gai_probe gai_probe.c)", cmake)
        self.assertIn("ws2_32", cmake)
        output = re.search(r'RUNTIME_OUTPUT_DIRECTORY "\$\{CMAKE_RUNTIME_OUTPUT_DIRECTORY\}/(\w+)"', cmake).group(1)
        self.assertEqual(systest.DEFAULT_GAI_PROBE, REPO / "build/mingw-release/bin" / output / "gai_probe.exe")
        self.assertIn("add_subdirectory(tools/winvm)", (REPO / "CMakeLists.txt").read_text())

    def test_scenario_gai_copies_the_built_probe_and_compiles_nothing(self):
        guest = mock.Mock()
        guest.ps.return_value = "gethostname=x\n"
        with tempfile.TemporaryDirectory(dir="/var/tmp") as work:
            probe = pathlib.Path(work) / "gai_probe.exe"
            probe.write_bytes(b"MZ")
            with mock.patch.object(systest.subprocess, "run") as run, \
                    mock.patch.object(systest.checks, "check_getaddrinfo", return_value="verdict") as check:
                result = systest.scenario_gai(guest, pathlib.Path(work), probe)
        run.assert_not_called()
        guest.put.assert_called_once_with(probe, f"{systest.SMB_ROOT}/run", "gai_probe.exe")
        check.assert_called_once_with("gethostname=x\n")
        self.assertEqual(result, ["verdict"])

    def test_a_missing_probe_is_an_environment_error_that_names_the_target(self):
        with self.assertRaises(systest.EnvError) as raised:
            systest.scenario_gai(mock.Mock(), pathlib.Path("out"), pathlib.Path("/nonexistent/gai_probe.exe"))
        self.assertIn("--target gai_probe", str(raised.exception))

    def test_main_stops_with_exit_two_before_the_guest_when_the_probe_is_missing(self):
        err = io.StringIO()
        with tempfile.TemporaryDirectory(dir="/var/tmp") as root:
            env = {"NEVR_WORK_ROOT": root, "WINVM_USER": "u", "WINVM_PASS": "p"}
            with mock.patch.dict("os.environ", env, clear=True), contextlib.redirect_stderr(err), \
                    mock.patch.object(systest, "Guest") as guest:
                code = systest.main(["--scenario", "gai", "--gai-probe", f"{root}/none/gai_probe.exe"])
        self.assertEqual(code, 2)
        self.assertIn("gai_probe", err.getvalue())
        guest.assert_not_called()

    def test_the_boot_scenario_does_not_need_the_probe(self):
        err = io.StringIO()
        with tempfile.TemporaryDirectory(dir="/var/tmp") as root:
            env = {"NEVR_WORK_ROOT": root, "WINVM_USER": "u", "WINVM_PASS": "p"}
            with mock.patch.dict("os.environ", env, clear=True), contextlib.redirect_stderr(err):
                code = systest.main(["--scenario", "boot", "--dll", f"{root}/none.dll",
                                     "--gai-probe", f"{root}/none/gai_probe.exe"])
        self.assertEqual(code, 2)
        self.assertNotIn("gai_probe", err.getvalue())

    @unittest.skipUnless(systest.DEFAULT_GAI_PROBE.exists(), "gai_probe.exe is not built (cmake --build --preset mingw-release --target gai_probe)")
    def test_the_built_probe_is_a_64_bit_console_program_that_imports_winsock(self):
        data = systest.DEFAULT_GAI_PROBE.read_bytes()
        self.assertEqual(data[:2], b"MZ")
        pe = int.from_bytes(data[0x3C:0x40], "little")
        self.assertEqual(data[pe:pe + 4], b"PE\0\0")
        self.assertEqual(int.from_bytes(data[pe + 4:pe + 6], "little"), 0x8664)
        subsystem = int.from_bytes(data[pe + 24 + 68:pe + 24 + 70], "little")
        self.assertEqual(subsystem, 3, "console subsystem, as the VM run reads its output")
        self.assertIn(b"ws2_32.dll", data.lower())


if __name__ == "__main__":
    unittest.main()
