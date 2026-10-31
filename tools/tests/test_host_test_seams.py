"""The host units' test seams cannot ship (docs/adr/0008).

src/runtime/host/ carries seams a test needs (a CreateEventW that fails, a record builder that
throws, an output sink, a nested-fault probe). They exist only under NEVR_HOST_TESTING, which only the
test-only library nevr_host_core defines. The target that builds the host into BugSplat64.dll must not
define it, so the seams are not in the shipped file.

The check that looks at what is compiled is tools/verify_host_seams.py: it compiles each host unit
without the define with the build's own flags and reads the object with nm. It needs the MinGW compiler
and a configured build, so `just test-auth-unit` runs it. This file tests the parts that need neither:
the scan for a stray definition, the flag stripper, the symbol filter, and the real tree.
"""

import re
import sys
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "tools"))

import verify_host_seams as seams  # noqa: E402

DEFINE = seams.DEFINE
CMAKE = "src/runtime/CMakeLists.txt"
ALLOWED_LINE = "    target_compile_definitions(nevr_host_core PUBLIC NEVR_HOST_TESTING)\n"


class StrayDefinitionTest(unittest.TestCase):
    def offenders(self, files):
        return seams.stray_definitions(files)

    def test_the_real_tree_defines_it_in_one_place(self):
        self.assertEqual(self.offenders(seams.tracked_text_files()), [])
        cmake = (REPO / CMAKE).read_text(encoding="utf-8")
        self.assertEqual(len(re.findall(rf"target_compile_definitions\(nevr_host_core PUBLIC {DEFINE}\)", cmake)), 1)
        # ... and that library is created inside the BUILD_TESTING block.
        before = cmake[:cmake.index("add_library(nevr_host_core")]
        self.assertGreater(before.rfind("if(BUILD_TESTING)"), before.rfind("endif()"))

    def test_the_allowed_line_and_the_tests_of_the_define_pass(self):
        files = {
            CMAKE: ALLOWED_LINE + f"# a comment about {DEFINE}\n",
            "src/runtime/host/x.cpp": f"#if defined({DEFINE})\nint seam;\n#endif\n#ifdef {DEFINE}\n#endif\n"
                                      f"// {DEFINE} in a comment\n#if defined({DEFINE})  // the seams\n#endif\n"
                                      f"#if defined({DEFINE}) /* the seams */\n#endif\n",
        }
        self.assertEqual(self.offenders(files), [])

    def test_every_other_spelling_is_reported(self):
        spellings = {
            "header define": ("src/runtime/host/game_targets.h", f"#define {DEFINE} 1\n"),
            "add_compile_definitions": (CMAKE, f"add_compile_definitions({DEFINE})\n"),
            "target_compile_definitions elsewhere": (CMAKE, f"target_compile_definitions(nevr_runtime PRIVATE {DEFINE})\n"),
            "add_compile_options": ("CMakeLists.txt", f"add_compile_options(-D{DEFINE})\n"),
            "a flag in a preset": ("CMakePresets.json", f'"CMAKE_CXX_FLAGS": "-D{DEFINE}"\n'),
            "a flag in a script": ("tools/build.sh", f"CXXFLAGS=-D{DEFINE} make\n"),
            "a gate that is always true": ("src/runtime/host/runtime_rendezvous.h", f"#if defined({DEFINE}) || 1\n#endif\n"),
            "an always-true condition between block comments": ("src/runtime/host/x.h",
                                                                f"#if defined({DEFINE}) /* a */ || 1 /* b */\n#endif\n"),
            "a condition that continues into a comment": ("src/runtime/host/x.h",
                                                          f"#if defined({DEFINE}) /* a\n || 1 */\n#endif\n"),
            "a gate with a condition after it": ("src/runtime/host/x.h", f"#if defined({DEFINE}) && defined(X)\n#endif\n"),
            "an #elif": ("src/runtime/host/x.h", f"#elif defined({DEFINE})\n"),
            "a negated gate": ("src/runtime/host/x.h", f"#ifndef {DEFINE}\n#endif\n"),
            "a #cmakedefine in a template": ("src/runtime/host/config.h.in", f"#cmakedefine {DEFINE}\n"),
            "a #define in a template": ("src/runtime/host/config.h.in", f"#define {DEFINE} 1\n"),
            "a #define in an .inl": ("src/runtime/host/seams.inl", f"#define {DEFINE} 1\n"),
            "a python build driver": ("tools/build_x.py", f'args = ["-D{DEFINE}"]\n'),
            "a second nevr_host_core line": (CMAKE, ALLOWED_LINE + ALLOWED_LINE.replace("PUBLIC", "PRIVATE")),
        }
        for label, (path, text) in spellings.items():
            with self.subTest(label):
                self.assertNotEqual(self.offenders({path: text}), [], label)


class CompileCommandTest(unittest.TestCase):
    def test_a_compiler_launcher_is_refused_and_the_real_compiler_gets_its_own_nm(self):
        with self.assertRaises(ValueError):
            seams.nm_tool("/usr/bin/ccache")
        with self.assertRaises(ValueError):
            seams.nm_tool("/usr/bin/sccache")
        self.assertEqual(seams.nm_tool("/usr/bin/x86_64-w64-mingw32-g++"), "/usr/bin/x86_64-w64-mingw32-nm")
        self.assertEqual(seams.nm_tool("/usr/bin/x86_64-w64-mingw32-gcc"), "/usr/bin/x86_64-w64-mingw32-nm")
        self.assertEqual(seams.real_compiler_argv(["/usr/bin/ccache", "/usr/bin/x86_64-w64-mingw32-g++", "-c", "x.cpp"]),
                         ["/usr/bin/x86_64-w64-mingw32-g++", "-c", "x.cpp"])
        self.assertEqual(seams.real_compiler_argv(["g++", "-c"]), ["g++", "-c"])

    def test_the_define_is_removed_in_every_spelling(self):
        argv = ["g++", f"-D{DEFINE}", "-DOTHER", "-D", DEFINE, f"-D{DEFINE}=1", "-c", "x.cpp"]
        stripped, removed = seams.strip_define(argv)
        self.assertEqual(removed, 3)
        self.assertEqual(stripped, ["g++", "-DOTHER", "-c", "x.cpp"])

    def test_a_command_outside_the_host_and_the_tests_must_not_carry_it(self):
        def entry(source, output):
            return {"file": str(REPO / source), "output": output, "command": f"g++ -D{DEFINE} -c {source}"}
        self.assertEqual(seams.stray_define_commands([
            entry("src/runtime/host/crash_capture.cpp", "src/runtime/CMakeFiles/nevr_host_core.dir/host/crash_capture.cpp.obj"),
            entry("src/runtime/lifecycle/game_image_guard.cpp",
                  "src/runtime/CMakeFiles/nevr_host_core.dir/lifecycle/game_image_guard.cpp.obj"),
            entry("src/runtime/tests/test_host_json.cpp", "src/runtime/CMakeFiles/test_host_json.dir/x.obj"),
        ]), [])
        stray = seams.stray_define_commands([
            entry("src/runtime/lifecycle/boot.cpp", "src/runtime/CMakeFiles/nevr_runtime.dir/lifecycle/boot.cpp.obj"),
            entry("src/runtime/host/crash_capture.cpp", "src/runtime/CMakeFiles/nevr_runtime.dir/host/crash_capture.cpp.obj"),
        ])
        self.assertEqual(len(stray), 2)


class DefineOnlySymbolTest(unittest.TestCase):
    def test_a_symbol_only_the_define_adds_must_say_it_is_a_seam(self):
        without = {"nevr_host::Real()": "T"}
        with_define = dict(without)
        with_define.update({
            "nevr_host::BootstrapDiagnostics::LogLockUnreliableForTest()": "T",
            "nevr_host::RuntimeRendezvous::Context() const": "T",
            "typeinfo for std::runtime_error": "R",
            "nevr_host::BootstrapDiagnostics::LogLockUnreliable()": "T",
            "nevr_host::BootstrapDiagnostics::Mystery()": "T",
        })
        self.assertEqual(seams.define_only_unnamed(with_define, without), [
            "nevr_host::BootstrapDiagnostics::LogLockUnreliable()",
            "nevr_host::BootstrapDiagnostics::Mystery()",
        ])

    def test_every_symbol_a_round_three_seam_added_is_named_as_one(self):
        for name in ("nevr_host::BootstrapDiagnostics::LogLockUnreliableForTest()",
                     "nevr_host::BootstrapDiagnostics::SetLogMutexNameForTest(wchar_t const*)",
                     "nevr_host::(anonymous namespace)::g_log_mutex_name_for_test",
                     "nevr_host::BootstrapDiagnostics::JournalClaimed() const",
                     "nevr_host::RuntimeRendezvous::Context() const"):
            self.assertIsNotNone(seams.SEAM_SYMBOL.search(name), name)


class SeamSymbolTest(unittest.TestCase):
    NM = (
        "0000000000000000 T nevr_host::DestinationName(nevr_host::DiagnosticDestination)\n"
        "00000000000007c0 T nevr_host::BootstrapDiagnostics::PrepareForTest(wchar_t const*)\n"
        "0000000000000000 b nevr_host::(anonymous namespace)::g_fail_writes\n"
        "0000000000000008 b nevr_host::(anonymous namespace)::g_capture_probe\n"
        "                 U SetEvent\n"
        "                 U InstallWaitProbe\n"
    )

    def test_a_defined_seam_is_found_and_an_import_is_not(self):
        found = seams.seam_symbols(self.NM)
        self.assertEqual(len(found), 3)
        self.assertTrue(all("U " not in line for line in found))

    def test_a_clean_object_has_none(self):
        self.assertEqual(seams.seam_symbols(self.NM.splitlines()[0]), [])

    # nm of an object whose unit calls two inline header functions (core/crash_send.h), as the MinGW compiler writes it: each
    # function has a COMDAT section symbol named by its mangled name and a defined symbol named by its demangled one. The
    # mangled text ends "...OffsetESt17...", which spells "test" without being one.
    INLINE_NM = (
        "0000000000000000 t .text$_ZN15nevr_crash_send17ParseModuleOffsetESt17basic_string_viewIcSt11char_traitsIcEEPNS_7AddressE\n"
        "0000000000000000 p .pdata$_ZN15nevr_crash_send17ParseModuleOffsetESt17basic_string_viewIcSt11char_traitsIcEEPNS_7AddressE\n"
        "0000000000000000 r .xdata$_ZN15nevr_crash_send17ParseModuleOffsetESt17basic_string_viewIcSt11char_traitsIcEEPNS_7AddressE\n"
        "0000000000000000 T nevr_crash_send::ParseModuleOffset(std::basic_string_view<char, std::char_traits<char> >, "
        "nevr_crash_send::Address*)\n"
    )

    def test_a_section_symbol_is_not_read_for_the_word_test_in_its_mangled_name(self):
        self.assertEqual(seams.seam_symbols(self.INLINE_NM), [])

    def test_the_demangled_symbol_of_an_inline_seam_is_still_found(self):
        nm = self.INLINE_NM + "0000000000000000 T nevr_crash_send::ReadDiskForTest()\n" \
                              "0000000000000000 t .text$_ZN15nevr_crash_send14ReadDiskForTestEv\n"
        self.assertEqual([line.split(None, 2)[2] for line in seams.seam_symbols(nm)], ["nevr_crash_send::ReadDiskForTest()"])


class SharedUnitSeamTest(unittest.TestCase):
    """crash_report.cpp is the one report writer the host, nevr_core, the Quest sentinel and two test programs compile; its
    ResetForTest cannot sit behind a define that only nevr_host_core carries, so the check names it."""

    NM = "0000000000006060 T nevr_crash::ResetForTest()\n0000000000006070 T nevr_crash::ForgetEverythingForTest()\n"

    def test_the_named_seam_of_its_own_unit_is_allowed_and_nothing_else_in_that_unit(self):
        found = seams.seam_symbols(self.NM, unit="crash_report.cpp")
        self.assertEqual([line.split(None, 2)[2] for line in found], ["nevr_crash::ForgetEverythingForTest()"])

    def test_the_same_name_in_another_unit_is_a_seam(self):
        self.assertEqual(len(seams.seam_symbols(self.NM, unit="crash_capture.cpp")), 2)
        self.assertEqual(len(seams.seam_symbols(self.NM)), 2)

    def test_the_named_seam_exists_in_the_unit_it_is_named_for(self):
        text = (REPO / "src" / "core" / "crash_report.cpp").read_text(encoding="utf-8")
        self.assertIn("void ResetForTest() {", text)
        for unit, names in seams.SHARED_UNIT_SEAMS.items():
            self.assertTrue(names, unit)
            for name, reason in names.items():
                self.assertTrue(reason, name)


class HelpersReadSeamsOnlyUnderTheDefineTest(unittest.TestCase):
    def test_the_helpers_read_the_seams_only_under_the_define(self):
        text = (REPO / "src" / "runtime" / "host" / "runtime_rendezvous.cpp").read_text(encoding="utf-8")
        for helper in ("FailEventCreate", "FailHookBytesRead", "InstallWaitMs"):
            body = re.search(rf"\n(?:bool|DWORD) {helper}\(\) \{{(.*?)\n\}}", text, re.S).group(1)
            self.assertTrue(re.search(rf"#if defined\({DEFINE}\).*Seams\(\).*#else.*#endif", body, re.S), helper)


if __name__ == "__main__":
    unittest.main()
