"""Where the crash report send and the host's own crash report are wired (#1689).

The behaviour is in the GTests (test_crash_send, test_host_crash_send, test_host_crash_report). This pins what only the
wiring decides: that the send starts in the lowest layer of each platform and before anything that could stop it, that
the host's own report is written only when the runtime is not there to write it, and that no real send runs under the
host test harnesses.
"""

import re
import unittest
from pathlib import Path

from tools.tests.test_runtime_lifecycle_invariants import extract_braced_function
from tools.verify_host_seams import DEFINE as HOST_TEST_SEAM_DEFINE

ROOT = Path(__file__).resolve().parents[2]


def source(path):
    """The file with its // comments removed, so prose cannot satisfy a check."""
    return re.sub(r"//[^\n]*", "", (ROOT / path).read_text())


class ThePcSendStartsInTheHostBeforeTheRuntimeLoads(unittest.TestCase):
    def test_the_detour_starts_the_send_before_it_loads_the_runtime(self):
        text = source("src/runtime/host/runtime_rendezvous.cpp")
        detour = extract_braced_function(text, "std::int64_t RuntimeRendezvous::GameMainWrapperDetour(std::int64_t command_line) {")
        self.assertIn("StartCrashSend(*self->diagnostics_);", detour)
        self.assertLess(detour.index("StartCrashSend("), detour.index("InitOnceExecuteOnce(&self->load_once_"))
        self.assertLess(detour.index("SnapshotModulesForCrashReport();"), detour.index("StartCrashSend("))

    def test_the_host_carries_no_runtime_dependency_for_it(self):
        # The host's imports are pinned (tools/verify_pe_contract.py): winhttp.dll is loaded by name, never imported.
        text = source("src/runtime/host/crash_send_winhttp.cpp")
        self.assertIn('LoadLibraryExW(L"winhttp.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32)', text)
        cmake = (ROOT / "src/runtime/CMakeLists.txt").read_text()
        link = re.search(r"target_link_libraries\(nevr_host PRIVATE ([^)]*)\)", cmake)
        self.assertIsNotNone(link)
        self.assertNotIn("winhttp", link.group(1).lower())

    def test_no_real_send_runs_under_the_host_test_harnesses(self):
        text = (ROOT / "src/runtime/host/crash_send_host.cpp").read_text()
        body = text[text.index("void StartCrashSend("):]
        # The define's name comes from the seam check: only that check and its test spell it (tools/verify_host_seams.py).
        gate = rf"#if defined\({HOST_TEST_SEAM_DEFINE}\)\s*(//[^\n]*\n\s*)*(static_cast<void>\(diagnostics\);\s*)?return;"
        self.assertRegex(body, gate)


class TheHostWritesItsOwnReportOnlyWithoutTheRuntime(unittest.TestCase):
    def test_create_report_writes_the_report_first_and_only_when_the_runtime_is_absent(self):
        text = source("src/runtime/host/host_entry.cpp")
        body = extract_braced_function(text, "void CreateReport(PEXCEPTION_POINTERS exception_pointers) noexcept {")
        self.assertIn("if (g_rendezvous.RuntimeApi() == nullptr) WriteCrashReport(exception_pointers);", body)
        self.assertLess(body.index("WriteCrashReport("), body.index("g_capture.Capture("))

    def test_the_platform_is_registered_at_attach_without_file_io(self):
        text = source("src/runtime/host/host_entry.cpp")
        attach = extract_braced_function(text, "void OnProcessAttach(HMODULE host_module, HMODULE game_module, bool launcher_mode) noexcept {")
        self.assertIn("PrepareCrashReport(&g_diagnostics);", attach)
        prepare = extract_braced_function(source("src/runtime/host/crash_report_host.cpp"),
                                          "void PrepareCrashReport(BootstrapDiagnostics* diagnostics) noexcept {")
        for call in ("CreateFile", "CreateDirectory", "K32EnumProcessModules", "LoadLibrary"):
            self.assertNotIn(call, prepare)

    def test_the_runtime_and_the_host_share_one_pc_report_description(self):
        runtime = source("src/runtime/lifecycle/crash_report_win.cpp")
        host = source("src/runtime/host/crash_report_host.cpp")
        for text in (runtime, host):
            self.assertIn("DescribeException(", text)
            self.assertIn("OpenReportFile", text)
        self.assertNotIn("EXCEPTION_ACCESS_VIOLATION", runtime)  # the table lives once, in crash_report_win_common.cpp


class TheQuestSendStartsInTheSentinelBeforeAnythingCanCrash(unittest.TestCase):
    def test_the_constructor_arms_the_crash_handlers_first(self):
        text = source("src/quest/sentinel/entry.cpp")
        ctor = extract_braced_function(text, "static void nevr_sentinel_ctor() {")
        self.assertIn("sentinel::Arm();", ctor)
        self.assertLess(ctor.index("sentinel::Arm();"), ctor.index("LogConstructorBegin("))
        self.assertLess(ctor.index("sentinel::Arm();"), ctor.index("CaptureUiLooper();"))

    def test_the_arm_step_starts_the_send_after_the_handlers_are_armed(self):
        text = source("src/quest/integration/production_steps.cpp")
        body = extract_braced_function(text, "void ArmCrashReporter() override {")
        self.assertLess(body.index("sentinel::Arm();"), body.index("StartCrashSend("))


if __name__ == "__main__":
    unittest.main()
