import re
import unittest
from pathlib import Path

from tools.tests.test_runtime_lifecycle_invariants import extract_braced_function, strip_comments


ROOT = Path(__file__).resolve().parents[2]


class BootPhaseLogging(unittest.TestCase):
    """#92: InitializeAfterGameImageGuard runs from DllMain under the loader lock. Once
    InitializeFunctionPointers() resolves EchoVR::WriteLog, Log() enters the game logger
    there. The N36 census in `just verify` only reads that function's own body, so the
    helpers it calls are checked here."""

    def test_patch_detour_reports_through_the_tee_during_boot(self):
        source = (ROOT / "src/runtime/hook/patching.h").read_text()
        body = extract_braced_function(source, "inline BOOL PatchDetour(")
        match = re.search(
            r"if\s*\(\s*BootLogTee::InBootPhase\(\)\s*\)\s*\{(?P<then>.*?)\}\s*else\s*\{(?P<other>.*?)\}",
            body,
            re.S,
        )
        self.assertIsNotNone(match, "PatchDetour must branch on BootLogTee::InBootPhase()")
        self.assertIn("BootLogTee::TeeFprintf(", match.group("then"))
        self.assertNotRegex(match.group("then").replace("TeeFprintf(", ""), r"\bLog\(")
        self.assertIn("Log(EchoVR::LogLevel::Warning", match.group("other"))
        outside = body.replace(match.group(0), "")
        self.assertNotRegex(outside, r"\bLog\(", "PatchDetour logs outside the else branch")

    def test_boot_helpers_in_initialize_do_not_call_log(self):
        source = (ROOT / "src/runtime/lifecycle/initialize.cpp").read_text()
        for signature in ("static void NoteBootHookResult(", "static void InstallBootDetour("):
            body = extract_braced_function(source, signature)
            self.assertNotRegex(body, r"\bLog\(", f"{signature} calls Log() under the loader lock")

    def test_boot_phase_flag_does_not_depend_on_the_file_opening(self):
        source = (ROOT / "src/runtime/log/boot_log_tee.cpp").read_text()
        init = extract_braced_function(source, "void BootLogTee::Init(")
        self.assertRegex(init.lstrip("{ \n"), r"^g_boot_phase\.store\(true")
        close = extract_braced_function(source, "void BootLogTee::Close(")
        self.assertIn("g_boot_phase.store(false", close)
        self.assertIn("bool BootLogTee::InBootPhase()", strip_comments(source))


if __name__ == "__main__":
    unittest.main()
