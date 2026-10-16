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
            r"if\s*\(\s*nevr_boot_log_tee::InBootPhase\(\)\s*\)\s*\{(?P<then>.*?)\}\s*else\s*\{(?P<other>.*?)\}",
            body,
            re.S,
        )
        self.assertIsNotNone(match, "PatchDetour must branch on nevr_boot_log_tee::InBootPhase()")
        self.assertIn("nevr_boot_log_tee::TeeFprintf(", match.group("then"))
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
        self.assertRegex(body, r"nevr_boot_log_tee::InBootPhase\(\)")
        before_else = body.split("} else", 1)[0]
        self.assertNotRegex(before_else, r"\bLog\(", "InstallGameMainHook logs before the InBootPhase branch ends")
        self.assertIn("nevr_boot_log_tee::TeeFprintf(", before_else)

    def test_log_filter_init_cleanup_failure_goes_through_the_tee(self):
        # nevr_builtin_log_filter::Init runs from InitializeAfterGameImageGuard under the loader
        # lock, so the failed-hook cleanup record must use the boot tee. The N36 census reads only
        # that function's own body and would not see a Log() here.
        source = (ROOT / "src/runtime/log/builtin_filter.cpp").read_text()
        body = extract_braced_function(source, "void nevr_builtin_log_filter::Init(")
        self.assertIn("MH_RemoveHook(g_hook_target)", body, "subject vanished: no failed-hook cleanup")
        match = re.search(r"if\s*\(\s*cleanup\s*!=\s*MH_OK\s*\)\s*\{(?P<b>[^{}]*)\}", body)
        self.assertIsNotNone(match, "the cleanup failure must be reported")
        self.assertIn("nevr_boot_log_tee::TeeFprintf(", match.group("b"))
        self.assertNotRegex(match.group("b"), r"\bLog\(", "the cleanup failure calls Log() under the loader lock")

    def test_xpid_boot_path_reports_through_the_tee_and_never_log_or_serverfatal(self):
        # PatchDscProvider and PatchProviderPrefixOvrOrg run from Initialize() under the loader lock,
        # before the CLI is parsed: Log() is not allowed and ServerFatal would only warn.
        source = strip_comments((ROOT / "src/runtime/patch/xpid_patch.cpp").read_text())
        self.assertGreater(source.count("nevr_boot_log_tee::TeeFprintf("), 4, "subject vanished: no tee records")
        self.assertNotRegex(source, r"\bLog\(", "xpid_patch.cpp calls Log() on the boot path")
        self.assertNotIn("ServerFatal(", source, "xpid_patch.cpp calls ServerFatal before g_isServer is known")

    def test_every_boot_failure_records_a_cause_and_the_server_refusal_names_it(self):
        for path in ("src/runtime/lifecycle/initialize.cpp", "src/runtime/patch/xpid_patch.cpp"):
            source = strip_comments((ROOT / path).read_text())
            flags = len(re.findall(r"g_bootHookFailed\s*=\s*true", source))
            causes = len(re.findall(r"\bNoteBootHookFailureCause\(", source))
            self.assertGreater(flags, 0, f"subject vanished: no g_bootHookFailed writes in {path}")
            self.assertGreaterEqual(causes, flags, f"{path} sets g_bootHookFailed without recording a cause")
        boot = strip_comments((ROOT / "src/runtime/lifecycle/boot.cpp").read_text())
        self.assertRegex(boot, r"if\s*\(\s*g_bootHookFailed\s*\)\s*\{\s*ServerFatal\([^;]*g_bootHookFailedCause")

    def test_every_boot_return_ends_the_boot_phase(self):
        # An early return that skips nevr_boot_log_tee::Close() leaves InBootPhase() true for the whole run.
        source = (ROOT / "src/runtime/lifecycle/initialize.cpp").read_text()
        body = extract_braced_function(source, "static VOID InitializeAfterGameImageGuard(")
        failure = re.search(r"if\s*\(\s*!Hooking::Initialize\(\)\s*\)\s*\{(?P<b>.*?)\n  \}", body, re.S)
        self.assertIsNotNone(failure)
        self.assertRegex(failure.group("b"), r"nevr_boot_log_tee::Close\(\)\s*;[^}]*return\s*;")

    def test_late_unload_records_go_through_log(self):
        # CSysDLL_GetSymbol runs at unload, long after nevr_boot_log_tee::Close(): TeeFprintf there is stderr only.
        source = (ROOT / "src/runtime/lifecycle/initialize.cpp").read_text()
        line = re.search(r"[^\n]*RadPluginShutdown of a platform DLL skipped[^\n]*", source).group(0)
        self.assertIn("Log(", line)
        self.assertNotIn("TeeFprintf", line)

    def test_boot_phase_flag_does_not_depend_on_the_file_opening(self):
        source = (ROOT / "src/runtime/log/boot_log_tee.cpp").read_text()
        init = extract_braced_function(source, "void nevr_boot_log_tee::Init(")
        self.assertRegex(init.lstrip("{ \n"), r"^g_boot_phase\.store\(true")
        close = extract_braced_function(source, "void nevr_boot_log_tee::Close(")
        self.assertIn("g_boot_phase.store(false", close)
        self.assertIn("bool nevr_boot_log_tee::InBootPhase()", strip_comments(source))


if __name__ == "__main__":
    unittest.main()
