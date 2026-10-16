import re
import unittest
from pathlib import Path

from tools.tests.test_runtime_lifecycle_invariants import extract_braced_function, strip_comments


ROOT = Path(__file__).resolve().parents[2]


class BootPhaseLogging(unittest.TestCase):
    """InitializeAfterGameImageGuard runs from DllMain under the loader lock, between
    nevr_boot_log_tee::Init() and Close(). Log() routes by that phase itself (#593): through the boot
    tee while it runs, through the game's CLog after. test_boot_log_routing drives the routing; these
    checks keep callers from branching on the phase by hand again."""

    def test_no_caller_branches_on_the_boot_phase(self):
        offenders = []
        for path in sorted((ROOT / "src").rglob("*")):
            if path.suffix not in (".h", ".cpp") or "legacy" in path.parts:
                continue
            if "InBootPhase" in strip_comments(path.read_text(errors="replace")):
                offenders.append(str(path.relative_to(ROOT)))
        self.assertEqual(offenders, [], "Log() knows the boot phase; callers must not branch on it")

    def test_patch_detour_reports_a_failed_hook_through_log(self):
        source = (ROOT / "src/runtime/hook/patching.h").read_text()
        body = strip_comments(extract_braced_function(source, "inline BOOL PatchDetour("))
        self.assertIn("Log(EchoVR::LogLevel::Warning", body)
        self.assertNotIn("TeeFprintf", body)

    def test_log_routes_through_the_boot_sink_before_the_game_logger(self):
        source = strip_comments((ROOT / "src/core/logging.cpp").read_text())
        body = extract_braced_function(source, "VOID Log(EchoVR::LogLevel level, const CHAR* format, ...)")
        self.assertIn("g_bootLogSink.load(", body)
        self.assertLess(body.index("g_bootLogSink.load("), body.index("EchoVR::WriteLog("),
                        "Log() must take the boot sink before the game logger")

    def test_the_boot_tee_owns_the_boot_sink(self):
        # The boot phase is exactly the window the tee's sink is set: from the first statement of Init()
        # to the first statement of Close().
        source = (ROOT / "src/runtime/log/boot_log_tee.cpp").read_text()
        init = strip_comments(extract_braced_function(source, "void nevr_boot_log_tee::Init("))
        self.assertRegex(init.lstrip("{ \n"), r"^SetBootLogSink\(&ReportBootLog\)")
        close = strip_comments(extract_braced_function(source, "void nevr_boot_log_tee::Close("))
        self.assertRegex(close.lstrip("{ \n"), r"^SetBootLogSink\(nullptr\)")

    def test_log_filter_init_cleanup_failure_is_reported(self):
        # nevr_builtin_log_filter::Init runs from InitializeAfterGameImageGuard, in the boot phase.
        source = (ROOT / "src/runtime/log/builtin_filter.cpp").read_text()
        body = extract_braced_function(source, "void nevr_builtin_log_filter::Init(")
        self.assertIn("MH_RemoveHook(g_hook_target)", body, "subject vanished: no failed-hook cleanup")
        match = re.search(r"if\s*\(\s*cleanup\s*!=\s*MH_OK\s*\)\s*\{(?P<b>[^{}]*)\}", body)
        self.assertIsNotNone(match, "the cleanup failure must be reported")
        self.assertRegex(match.group("b"), r"nevr_boot_log_tee::TeeFprintf\(|\bLog\(")

    def test_log_filter_init_hook_outcomes_use_only_the_boot_tee(self):
        source = (ROOT / "src/runtime/log/builtin_filter.cpp").read_text()
        body = extract_braced_function(source, "void nevr_builtin_log_filter::Init(")
        self.assertEqual(
            2,
            len(re.findall(
                r'nevr_boot_log_tee::TeeFprintf\(\s*"\[NEVR.PATCH\] warn; hook failed name=CLog::PrintfImpl',
                body,
            )),
            "both failed hook stages must report through the boot tee",
        )
        self.assertEqual(2, body.count('"current=%02x%02x%02x%02x status=%s\\n"'))
        self.assertRegex(
            body,
            r'nevr_boot_log_tee::TeeFprintf\(\s*"\[NEVR.PATCH\] debug; hooked name=CLog::PrintfImpl va=0x%llX\\n"',
        )
        self.assertNotRegex(body, r"\bLog\s*\(", "Init() runs in the boot phase (N36): its records go to the boot tee, not Log()")

    def test_xpid_boot_path_reports_through_the_tee_and_never_serverfatal(self):
        # PatchDscProvider and PatchProviderPrefixOvrOrg run from Initialize(), before the CLI is
        # parsed: ServerFatal would only warn.
        source = strip_comments((ROOT / "src/runtime/patch/xpid_patch.cpp").read_text())
        self.assertGreater(source.count("nevr_boot_log_tee::TeeFprintf("), 4, "subject vanished: no tee records")
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
        # An early return that skips nevr_boot_log_tee::Close() leaves Log() on the boot tee for the whole run.
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


if __name__ == "__main__":
    unittest.main()
