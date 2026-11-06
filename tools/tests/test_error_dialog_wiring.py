import unittest
from pathlib import Path

from tools.tests.test_runtime_lifecycle_invariants import extract_braced_function, strip_comments


ROOT = Path(__file__).resolve().parents[2]


class ErrorDialogWiring(unittest.TestCase):
    """#2245. A line the game logs from its error dialog (echovr.exe 0x1400EA730) is recorded at error. The behaviour is
    test_error_dialog_depth.cpp's and test_run_log_record.cpp's; builtin_filter.cpp and crash_recovery.cpp hook the game and
    link no test binary, so these checks pin that the pieces those tests drive are the ones the hooks call."""

    def setUp(self):
        self.filter = strip_comments((ROOT / "src/runtime/log/builtin_filter.cpp").read_text())
        self.dialog = strip_comments((ROOT / "src/runtime/log/error_dialog_level.cpp").read_text())
        self.crash = strip_comments((ROOT / "src/runtime/lifecycle/crash_recovery.cpp").read_text())

    def test_the_filter_hook_asks_whether_the_line_comes_from_the_dialog(self):
        hook = extract_braced_function(self.filter, "static void __fastcall hook_PrintfImpl(")
        self.assertIn("const bool insideErrorDialog = nevr_error_dialog::Inside();", hook)
        self.assertIn("const uint32_t effective = nevr_log_level::Effective(level, insideErrorDialog);", hook)
        self.assertLess(hook.index("nevr_error_dialog::Inside()"), hook.index("ShouldSuppress(buf, effective, &entry)"),
                        "the level gate sees the level the line is recorded at")
        self.assertIn("EmitLine(level, insideErrorDialog, buf, emit_len);", hook,
                      "the record gets the game's level and the mark, so it writes error and game_level")

    def test_the_level_gate_is_the_one_the_tests_drive(self):
        should = extract_braced_function(self.filter, "static bool ShouldSuppress(")
        self.assertIn("nevr_log_level::DroppedByMinLevel(level, g_config.min_level)", should)
        self.assertNotIn("level < g_config.min_level", should, "no second copy of the rule")

    def test_the_filter_installs_the_dialog_hook_once_its_own_hook_is_in_place(self):
        init = extract_braced_function(self.filter, "void nevr_builtin_log_filter::Init(")
        self.assertEqual(init.count("nevr_error_dialog::Install(base_addr);"), 1)
        self.assertLess(init.index('nevr_hook_guard::Record(g_hook_target, "CLog::PrintfImpl");'),
                        init.index("nevr_error_dialog::Install(base_addr);"))

    def test_the_dialog_hook_runs_the_original_through_the_marked_scope(self):
        hook = extract_braced_function(self.dialog, "void ErrorDialogHook(")
        self.assertIn("Run([&] { g_original(owner, message, title); });", hook)
        self.assertNotIn("t_depth", hook, "the mark is set and cleared only by Run")

    def test_the_longjmp_landing_clears_a_mark_the_longjmp_left(self):
        # The VEH longjmps out of the game loop without running destructors (crash_recovery.cpp, N70 item 3), so the
        # landing branch clears the mark itself.
        wrapper = extract_braced_function(self.crash, "static INT64 GameMainWrapperHook(")
        landing = wrapper[wrapper.index("if (crashCount > 0) {"):]
        self.assertIn("nevr_error_dialog::ResetAfterLongjmp();", landing[:landing.index("Log(")],
                      "cleared before the landing logs anything")


if __name__ == "__main__":
    unittest.main()
