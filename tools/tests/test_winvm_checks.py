#!/usr/bin/env python3
"""Tests for the Windows-VM system test's pass/fail logic (tools/winvm/checks.py).

The fixtures are artifacts from real runs on a native Windows 11 guest
(2026-09-21). dbgcore_fatal.txt is condensed from the real log (non-ASCII
dashes normalised); the rest are verbatim.

Each failure-mode test is a failure that actually cost time: a game blocked on a
modal dialog was first mistaken for a getaddrinfo hang, because from outside it
looks identical (alive, Responding, ~0 CPU, silent log).
"""

import pathlib
import sys
import unittest

REPO = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "tools" / "winvm"))

import checks  # noqa: E402

FIX = pathlib.Path(__file__).resolve().parent / "fixtures" / "winvm"


def fixture(name: str) -> str:
    return (FIX / name).read_text(encoding="utf-8", errors="replace")


def by_name(results, name):
    return [r for r in results if r.name == name]


class ModalDialogTest(unittest.TestCase):
    def test_echo_relay_dialog_is_a_failure_that_names_the_message(self):
        r = checks.check_no_modal_dialog(fixture("echo_relay_dialog_windows.txt"))
        self.assertEqual(r.status, checks.FAIL)
        self.assertIn("Echo Relay: Error", r.detail)
        self.assertIn("-noconsole can only be used with the -headless argument.", r.detail)

    def test_ime_windows_are_not_dialogs(self):
        r = checks.check_no_modal_dialog(fixture("no_dialog_windows.txt"))
        self.assertEqual(r.status, checks.PASS)

    def test_empty_dump_passes(self):
        self.assertEqual(checks.check_no_modal_dialog("").status, checks.PASS)


class FatalTest(unittest.TestCase):
    def test_dbgcore_hijack_fatal_is_reported(self):
        r = checks.check_no_fatal(fixture("dbgcore_fatal.txt"), exit_code=1)
        self.assertEqual(r.status, checks.FAIL)
        self.assertIn("dbgcore.dll hijack detected", r.detail)

    def test_healthy_log_has_no_fatal(self):
        r = checks.check_no_fatal(fixture("healthy_boot.txt"), exit_code=None)
        self.assertEqual(r.status, checks.PASS)

    def test_silent_nonzero_exit_is_still_a_failure(self):
        r = checks.check_no_fatal("[NEVR.PATCH] boot log opened\n", exit_code=3)
        self.assertEqual(r.status, checks.FAIL)
        self.assertIn("rc=3", r.detail)


class HooksTest(unittest.TestCase):
    def test_healthy_boot_passes_and_warns_about_known_failures(self):
        results = checks.check_hooks(fixture("healthy_boot.txt"))
        self.assertEqual(by_name(results, "hooks_installed")[0].status, checks.PASS)
        self.assertEqual(by_name(results, "no_unexpected_hook_failure")[0].status, checks.PASS)
        warned = {r.detail.split(": ")[0] for r in by_name(results, "known_hook_failure")}
        self.assertEqual(warned, {"EchoVR::GetProcAddress", "LoadLibraryW", "LoadLibraryExW"})
        self.assertTrue(checks.overall(results))

    def test_unknown_hook_failure_fails(self):
        log = ("info All hooks installed\n"
               "warn [NEVR.PATCH] hook FAILED name=SomethingNew target=0x1 reason=MH_ERROR_UNSUPPORTED_FUNCTION\n")
        results = checks.check_hooks(log)
        bad = by_name(results, "no_unexpected_hook_failure")[0]
        self.assertEqual(bad.status, checks.FAIL)
        self.assertIn("SomethingNew", bad.detail)
        self.assertFalse(checks.overall(results))

    def test_missing_all_hooks_installed_fails(self):
        self.assertEqual(by_name(checks.check_hooks("nothing useful\n"), "hooks_installed")[0].status,
                         checks.FAIL)


class EngineProgressTest(unittest.TestCase):
    def test_healthy_boot_reaches_the_broadcaster(self):
        log = fixture("healthy_boot.txt")
        self.assertEqual(checks.engine_stage_reached(log), "broadcaster")
        self.assertEqual(checks.check_engine_progress(log, "broadcaster").status, checks.PASS)

    def test_run_that_stalls_after_the_banner_reports_where(self):
        # Exactly what the dialog-blocked run's log looked like: the runtime's own
        # lines and the engine banner, then silence.
        log = fixture("healthy_boot.txt")
        cut = log.index("Echo VR\n") + len("Echo VR\n")
        stalled = log[:cut]
        self.assertEqual(checks.engine_stage_reached(stalled), "banner")
        r = checks.check_engine_progress(stalled, "config_loaded")
        self.assertEqual(r.status, checks.FAIL)
        self.assertIn("stopped at 'banner'", r.detail)

    def test_stall_before_the_broadcaster_is_the_issue_13_signature(self):
        # Booted past SYSNET but CBroadcaster::Initialize never returned: no
        # Listen entries in the periodic report.
        log = fixture("healthy_boot.txt")
        cut = log.index("[SYSNET] Found Internet connection") + 40
        r = checks.check_engine_progress(log[:cut], "broadcaster")
        self.assertEqual(r.status, checks.FAIL)
        self.assertIn("stopped at 'sysnet'", r.detail)

    def test_listen_with_zero_entries_does_not_count(self):
        log = "hook_liveness name=CBroadcaster::Listen entries=0 entered=NO expected=registration\n"
        self.assertIsNone(checks.engine_stage_reached(log))

    def test_unknown_required_stage_is_a_programming_error(self):
        with self.assertRaises(ValueError):
            checks.check_engine_progress("", "no_such_stage")


class ProcessAliveTest(unittest.TestCase):
    def test_alive_when_expected(self):
        self.assertEqual(checks.check_process_alive(True, None).status, checks.PASS)

    def test_exit_when_alive_expected_fails_with_the_code(self):
        r = checks.check_process_alive(False, 1)
        self.assertEqual(r.status, checks.FAIL)
        self.assertIn("1", r.detail)

    def test_expect_exit(self):
        self.assertEqual(checks.check_process_alive(False, 0, "exit").status, checks.PASS)
        self.assertEqual(checks.check_process_alive(True, None, "exit").status, checks.FAIL)


class GetAddrInfoTest(unittest.TestCase):
    def test_real_probe_output_passes(self):
        r = checks.check_getaddrinfo(fixture("gai_probe_output.txt"))
        self.assertEqual(r.status, checks.PASS)
        self.assertIn("slowest 5.9 ms", r.detail)

    def test_slow_resolution_fails(self):
        out = 'game call: node="" flags=0         rc=0 wsaerr=0 elapsed=31000.0 ms -> 10.0.0.5\n'
        r = checks.check_getaddrinfo(out)
        self.assertEqual(r.status, checks.FAIL)
        self.assertIn("31000", r.detail)

    def test_failed_resolution_fails(self):
        out = 'game call: node="" flags=0         rc=11001 wsaerr=11001 elapsed=12.0 ms\n'
        self.assertEqual(checks.check_getaddrinfo(out).status, checks.FAIL)

    def test_no_rows_fails_rather_than_passing_vacuously(self):
        self.assertEqual(checks.check_getaddrinfo("").status, checks.FAIL)


if __name__ == "__main__":
    unittest.main()
