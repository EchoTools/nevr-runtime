"""The host loads dbghelp.dll before any crash, never inside the crash callback (#803, ADR 0008).

echovr.exe imports no dbghelp and the game's HandleCrashDump calls no loader function, so nothing else loads it:
a forced crash in a game copy recorded crash_dump outcome=dbghelp_unavailable, win32_error=126. The host therefore
loads the system dbghelp.dll once, from the thread OnProcessAttach starts (a thread, because DllMain holds the loader
lock), for both the direct-import and the launcher attach. The crash callback itself still calls no loader function:
it runs in a faulting process and may hold or wait on the loader lock.
"""
from __future__ import annotations

import pathlib
import re
import unittest

REPO = pathlib.Path(__file__).resolve().parents[2]
CAPTURE = REPO / "src/runtime/host/crash_capture.cpp"
ENTRY = REPO / "src/runtime/host/host_entry.cpp"


def code(text: str) -> str:
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    return re.sub(r"//[^\n]*", "", text)


def body(text: str, signature: str) -> str:
    source = code(text)
    match = re.search(signature + r"[^{;]*\{", source)
    if match is None:
        raise AssertionError(f"no definition matching {signature}")
    depth, i = 1, match.end()
    while depth and i < len(source):
        depth += {"{": 1, "}": -1}.get(source[i], 0)
        i += 1
    return source[match.end():i - 1]


class DbgHelpPreloadTest(unittest.TestCase):
    def test_the_crash_callback_and_its_helpers_call_no_loader_function(self):
        text = CAPTURE.read_text()
        for signature in (r"bool CrashCapture::Capture\(", r"CrashCapture::Outcome CrashCapture::WriteDumpTo\(",
                          r"void CrashCapture::WriteTextDiagnostic\("):
            with self.subTest(signature):
                self.assertNotRegex(body(text, signature), r"LoadLibrary|LdrLoadDll|LoadPackagedLibrary")

    def test_the_preload_is_a_system32_only_load_of_dbghelp(self):
        text = body(CAPTURE.read_text(), r"CrashCapture::PreloadDbgHelp\(\)")
        self.assertRegex(text, r"LoadLibraryExW\(\s*kDbgHelpModule\s*,\s*nullptr\s*,\s*LOAD_LIBRARY_SEARCH_SYSTEM32\s*\)")
        self.assertIn("GetModuleHandleW(kDbgHelpModule)", text, "an already-loaded dbghelp must not be loaded again")

    def test_a_failed_preload_is_recorded(self):
        text = body(CAPTURE.read_text(), r"CrashCapture::PreloadDbgHelp\(\)")
        self.assertRegex(text, r'JsonLine\s+\w+\(\s*\w+\s*,\s*sizeof\(\w+\)\s*,\s*"dbghelp_preload"\s*\)')
        self.assertIn("GetLastError()", text)

    def test_attach_starts_the_preload_for_both_attach_modes(self):
        text = body(ENTRY.read_text(), r"void OnProcessAttach\(")
        start = text.find("StartDbgHelpPreload")
        launcher_return = text.find("if (launcher_mode) return;")
        self.assertNotEqual(start, -1, "OnProcessAttach never starts the dbghelp preload")
        self.assertNotEqual(launcher_return, -1)
        self.assertLess(start, launcher_return, "the launcher attach returns before the preload starts")

    def test_the_preload_runs_on_its_own_thread_not_under_the_loader_lock(self):
        text = body(ENTRY.read_text(), r"void StartDbgHelpPreload\(\)")
        self.assertIn("CreateThread", text)
        self.assertNotIn("PreloadDbgHelp()", text, "the preload must run on the thread, not on the attach thread")
        self.assertIn("PreloadDbgHelp()", body(ENTRY.read_text(), r"DWORD WINAPI PreloadDbgHelpThread\("))


if __name__ == "__main__":
    unittest.main()
