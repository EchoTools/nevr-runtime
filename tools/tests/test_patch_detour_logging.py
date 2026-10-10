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

    def test_game_main_hook_reports_through_the_tee_during_boot(self):
        # InstallGameMainHook runs in the boot phase (initialize.cpp), under the loader lock.
        source = (ROOT / "src/runtime/lifecycle/crash_recovery.cpp").read_text()
        body = extract_braced_function(source, "void InstallGameMainHook(")
        self.assertRegex(body, r"BootLogTee::InBootPhase\(\)")
        before_else = body.split("} else", 1)[0]
        self.assertNotRegex(before_else, r"\bLog\(", "InstallGameMainHook logs before the InBootPhase branch ends")
        self.assertIn("BootLogTee::TeeFprintf(", before_else)

    def test_every_boot_return_ends_the_boot_phase(self):
        # An early return that skips BootLogTee::Close() leaves InBootPhase() true for the whole run.
        source = (ROOT / "src/runtime/lifecycle/initialize.cpp").read_text()
        body = extract_braced_function(source, "static VOID InitializeAfterGameImageGuard(")
        failure = re.search(r"if\s*\(\s*!Hooking::Initialize\(\)\s*\)\s*\{(?P<b>.*?)\n  \}", body, re.S)
        self.assertIsNotNone(failure)
        self.assertRegex(failure.group("b"), r"BootLogTee::Close\(\)\s*;[^}]*return\s*;")

    def test_late_unload_records_go_through_log(self):
        # CSysDLL_GetSymbol runs at unload, long after BootLogTee::Close(): TeeFprintf there is stderr only.
        source = (ROOT / "src/runtime/lifecycle/initialize.cpp").read_text()
        line = re.search(r"[^\n]*RadPluginShutdown of a platform DLL skipped[^\n]*", source).group(0)
        self.assertIn("Log(", line)
        self.assertNotIn("TeeFprintf", line)

    def test_boot_phase_flag_does_not_depend_on_the_file_opening(self):
        source = (ROOT / "src/runtime/log/boot_log_tee.cpp").read_text()
        init = extract_braced_function(source, "void BootLogTee::Init(")
        self.assertRegex(init.lstrip("{ \n"), r"^g_boot_phase\.store\(true")
        close = extract_braced_function(source, "void BootLogTee::Close(")
        self.assertIn("g_boot_phase.store(false", close)
        self.assertIn("bool BootLogTee::InBootPhase()", strip_comments(source))


if __name__ == "__main__":
    unittest.main()
