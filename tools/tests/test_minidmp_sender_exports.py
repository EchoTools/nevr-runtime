"""The shipped BugSplat64.dll's MiniDmpSender::createReport writes the host's minidump (#803, ADR 0008, ADR 0012 step 1b).

The game's HandleCrashDump builds a MiniDmpSender and calls createReport(exception pointers). That export is
BugSplat_createReport (src/runtime/host/exports.def). It must call nevr_host::CreateReport, there must be exactly one
definition of it in the product, and the crash harness must link that definition instead of defining its own,
so the harness drives the code the DLL exports.
"""
from __future__ import annotations

import pathlib
import re
import unittest

REPO = pathlib.Path(__file__).resolve().parents[2]
EXPORTS_CPP = REPO / "src/runtime/host/minidmp_sender_exports.cpp"
CMAKE = REPO / "src/runtime/CMakeLists.txt"
SHIM = REPO / "src/runtime/tests/host_crash/shim.cpp"
DLLMAIN = REPO / "src/runtime/lifecycle/dllmain.cpp"
EXPORTS_DEF = REPO / "src" / "runtime" / "host" / "exports.def"
NINE = ("BugSplat_MiniDmpSender_Ctor", "BugSplat_MiniDmpSender_Dtor", "BugSplat_createReport", "BugSplat_getFlags",
        "BugSplat_resetAppIdentifier", "BugSplat_sendAdditionalFile", "BugSplat_setDefaultUserDescription",
        "BugSplat_setDefaultUserName", "BugSplat_setFlags")


def code(text: str) -> str:
    """The source without comments, so prose cannot satisfy or break a match."""
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    return re.sub(r"//[^\n]*", "", text)


def body(text: str, name: str) -> str:
    match = re.search(rf"\b{name}\s*\([^)]*\)\s*\{{", code(text))
    if match is None:
        raise AssertionError(f"no definition of {name}")
    source = code(text)
    depth, i = 1, match.end()
    while depth and i < len(source):
        depth += {"{": 1, "}": -1}.get(source[i], 0)
        i += 1
    return source[match.end():i - 1]


class CreateReportWiringTest(unittest.TestCase):
    def test_the_export_calls_the_host_capture(self):
        self.assertRegex(body(EXPORTS_CPP.read_text(), "BugSplat_createReport"),
                         r"nevr_host::CreateReport\s*\(\s*\w+\s*\)")

    def test_the_export_hands_the_exception_pointers_it_was_given_to_the_capture(self):
        text = body(EXPORTS_CPP.read_text(), "BugSplat_createReport")
        parameter = re.search(r"BugSplat_createReport\s*\(\s*void\s*\*\s*\w*\s*,\s*PEXCEPTION_POINTERS\s+(\w+)\s*\)",
                              code(EXPORTS_CPP.read_text()))
        self.assertIsNotNone(parameter, "the second parameter of BugSplat_createReport is unnamed")
        self.assertIn(f"CreateReport({parameter.group(1)})", re.sub(r"\s+", "", text))

    def test_every_export_the_game_imports_is_defined_once_in_that_file(self):
        text = code(EXPORTS_CPP.read_text())
        for name in NINE:
            with self.subTest(name):
                self.assertEqual(len(re.findall(rf"\b{name}\s*\(", text)), 1)

    def test_the_dll_and_the_harness_do_not_define_their_own(self):
        for path in (DLLMAIN, SHIM):
            text = code(path.read_text())
            for name in NINE:
                with self.subTest(file=path.name, name=name):
                    self.assertIsNone(re.search(rf"\b{name}\s*\([^)]*\)\s*\{{", text),
                                      f"{path.name} defines {name}; the one definition is {EXPORTS_CPP.name}")

    def test_the_dll_compiles_that_file_and_the_harness_gets_it_from_the_host_library(self):
        cmake = CMAKE.read_text()
        # BugSplat64.dll is the host (#853): the game imports these nine from it, so the host's units hold the file
        # and the runtime's sources do not.
        units = re.search(r"set\(NEVR_HOST_UNITS(.*?)\n\)", cmake, flags=re.S)
        self.assertIsNotNone(units)
        self.assertIn('"host/minidmp_sender_exports.cpp"', units.group(1))
        host = re.search(r"add_library\(nevr_host SHARED(.*?)\)", cmake, flags=re.S)
        self.assertIsNotNone(host)
        self.assertIn("${NEVR_HOST_UNITS}", host.group(1))
        sources = re.search(r"set\(PATCHES_SOURCES(.*?)\n\)", cmake, flags=re.S)
        self.assertIsNotNone(sources)
        self.assertNotIn("minidmp_sender_exports", sources.group(1))
        # The host library holds the host units the harnesses link (tools/verify_host_seams.py reads it).
        library = re.search(r"add_library\(nevr_host_core STATIC(.*?)\)", cmake, flags=re.S)
        self.assertIsNotNone(library)
        self.assertIn("${NEVR_HOST_UNITS}", library.group(1))
        shim = re.search(r"add_library\(test_host_crash_shim SHARED(.*?)\)", cmake, flags=re.S)
        self.assertIsNotNone(shim)
        self.assertNotIn("minidmp_sender_exports", shim.group(1), "the shim must link the host library's copy")
        self.assertIn("target_link_libraries(test_host_crash_shim PRIVATE nevr_host_core)", cmake)
        # shim.def names the symbol and an archive member is only taken for an undefined symbol.
        self.assertIn("target_link_options(test_host_crash_shim PRIVATE -Wl,--undefined=BugSplat_createReport)", cmake)

    def test_the_export_table_still_maps_the_mangled_name_to_it(self):
        self.assertIn("?createReport@MiniDmpSender@@QEAAXPEAU_EXCEPTION_POINTERS@@@Z = BugSplat_createReport",
                      EXPORTS_DEF.read_text())


if __name__ == "__main__":
    unittest.main()
