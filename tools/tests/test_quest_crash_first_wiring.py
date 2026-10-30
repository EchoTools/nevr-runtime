"""#1605: the sentinel's crash handler stays first, and the game's later sigaction cannot displace it.

sentinel.cpp, entry.cpp and production_steps.cpp are the device's wiring (no host recipe compiles them), so the calls they
must make are pinned by source shape here; the behaviour behind them is tested on the host (crash_first_hook_test,
integration_sequence_test) and the two slots are resolved in the real library by got_pinned_test.
"""

import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
QUEST = ROOT / "src/quest"


def code(rel: str) -> str:
    text = (QUEST / rel).read_text()
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    return re.sub(r"//[^\n]*", "", text)


def body(text: str, signature: str) -> str:
    start = text.index(signature)
    open_at = text.index("{", start)
    depth = 0
    for i in range(open_at, len(text)):
        depth += {"{": 1, "}": -1}.get(text[i], 0)
        if depth == 0:
            return text[open_at : i + 1]
    raise AssertionError(f"unbalanced body for {signature}")


class CrashHandlerIsOurs(unittest.TestCase):
    def test_breakpad_installs_no_handlers_of_its_own_and_ours_are_installed_after_it_is_built(self):
        arm = body((QUEST / "sentinel/sentinel.cpp").read_text(), "void Arm()")  # raw: the argument is named by a comment
        self.assertIn("/*install_handler=*/false", arm)
        self.assertNotIn("/*install_handler=*/true", arm)
        built = arm.index("new google_breakpad::ExceptionHandler")
        installed = arm.index("nevr_quest::crash_first::InstallHandlers(")
        self.assertLess(built, installed)

    def test_the_crash_path_is_report_then_dump_then_the_games_handler(self):
        hook = code("sentinel/crash_first_hook.cpp")
        on_signal = body(hook, "void OnSignal(")
        order = [on_signal.index(token) for token in ("g_actions.report(", "g_actions.dump(", "RestoreHandlers();", "ChainToGame(")]
        self.assertEqual(order, sorted(order))
        self.assertIn("g_claim.TryClaim()", on_signal)

    def test_the_actions_are_the_report_the_breakpad_dump_and_one_line(self):
        sentinel = code("sentinel/sentinel.cpp")
        self.assertIn("nevr_crash::WriteReport(crash)", body(sentinel, "void ReportAction("))
        self.assertIn("g_handler->HandleSignal(", body(sentinel, "void DumpAction("))
        self.assertIn("InstallHandlers({&ReportAction, &DumpAction, &NoteAction})", sentinel)

    def test_disarm_puts_the_system_handlers_back(self):
        self.assertIn("crash_first::RestoreHandlers()", body(code("sentinel/sentinel.cpp"), "void Disarm()"))


class TheGamesRequestsAreIntercepted(unittest.TestCase):
    def test_both_libc_slots_of_libr15_are_pinned_by_symbol_and_slot(self):
        pinned = code("sentinel/pinned_targets.h")
        self.assertIn("0x36f3b68ULL", body(pinned, "inline GotTarget LibR15Sigaction()"))
        self.assertIn("kSigactionSymbol", body(pinned, "inline GotTarget LibR15Sigaction()"))
        self.assertIn("0x36e6508ULL", body(pinned, "inline GotTarget LibR15Signal()"))
        self.assertIn("kSignalSymbol", body(pinned, "inline GotTarget LibR15Signal()"))
        self.assertIn('kSigactionSymbol = "sigaction"', pinned)
        self.assertIn('kSignalSymbol = "signal"', pinned)

    def test_the_hook_installs_both_thunks_on_the_pinned_targets_and_never_logs_from_a_handler(self):
        hook = code("sentinel/crash_first_hook.cpp")
        install = body(hook, "bool Install() noexcept")
        self.assertIn("LibR15Sigaction()", install)
        self.assertIn("LibR15Signal()", install)
        for handler in ("int SigactionHandler(", "SignalHandlerFn SignalHandler("):
            text = body(hook, handler)
            self.assertNotIn("LogFields", text)
            self.assertNotIn("__android_log", text)
        # A guarded signal's request is answered here and never reaches libc.
        guarded = body(hook, "int SigactionHandler(")
        self.assertEqual(guarded.count("original("), 1)

    def test_the_sequence_runs_the_step_after_process_end_and_the_production_step_installs_through_the_seam(self):
        seq = code("integration/ctor_sequence.cpp")
        order = [seq.index(f"r.Run(StepId::{step}") for step in ("kInstallProcessEnd", "kInstallCrashFirst", "kInstallPermissionWait")]
        self.assertEqual(order, sorted(order))
        self.assertIn('"install_crash_first"', seq)
        self.assertIn("virtual bool InstallCrashFirst() = 0;", code("integration/ctor_sequence.h"))
        step = body(code("integration/production_steps.cpp"), "bool InstallCrashFirst() override")
        self.assertIn("nevr_quest::integration::InstallCrashFirstHook()", step)
        entry = code("sentinel/entry.cpp")
        self.assertIn("bool InstallCrashFirstHook() noexcept { return nevr_quest::crash_first::Install(); }", entry)

    def test_the_hook_is_built_without_exceptions_and_the_host_test_runs_in_test_quest_hooks(self):
        cmake = (QUEST / "sentinel/CMakeLists.txt").read_text()
        self.assertEqual(cmake.count("crash_first_hook.cpp"), 2)  # a source, and the -fno-exceptions list
        justfile = (ROOT / "justfile").read_text()
        self.assertIn("src/quest/tests/crash_first_hook_test.cpp", justfile)
        self.assertIn("timeout 120 \"$out/crash_first_hook_test\"", justfile)


if __name__ == "__main__":
    unittest.main()
