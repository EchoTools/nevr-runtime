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

    def test_actual_lowercase_required_emitter_fails_even_after_success_banner(self):
        log = ("[NEVR.PATCH] All hooks installed\n"
               "[NEVR.PATCH] hook failed name=CSysDLL_Load\n")
        results = checks.check_hooks(log)
        self.assertEqual(by_name(results, "hooks_installed")[0].status, checks.PASS)
        self.assertEqual(by_name(results, "no_unexpected_hook_failure")[0].status, checks.FAIL)

    def test_result_failed_emitter_fails(self):
        results = checks.check_hooks("[NEVR.PATCH] hook name=PreprocessCommandLine result=FAILED\n")
        self.assertEqual(by_name(results, "no_unexpected_hook_failure")[0].status, checks.FAIL)

    def test_headless_hook_failure_is_required(self):
        results = checks.check_hooks("[NEVR.HEADLESS] hook failed name=D3D12CreateDevice va=0x1 expected=0 actual=1\n")
        self.assertEqual(by_name(results, "no_unexpected_hook_failure")[0].status, checks.FAIL)

    def test_explicit_diag_failure_is_warning_but_same_name_without_diag_fails(self):
        diag = checks.check_hooks("[NEVR.PATCH] DIAG hook failed name=NetGameHostCheck va=0x1 reason=prologue_mismatch\n")
        self.assertEqual(by_name(diag, "diagnostic_hook_failure")[0].status, checks.WARN)
        plain = checks.check_hooks("[NEVR.PATCH] hook failed name=NetGameHostCheck va=0x1 reason=prologue_mismatch\n")
        self.assertEqual(by_name(plain, "no_unexpected_hook_failure")[0].status, checks.FAIL)

    def test_known_exception_requires_expected_status_reason_and_provenance(self):
        known = ("[NEVR.PATCH] hook FAILED name=EchoVR::GetProcAddress target=0x1 "
                 "reason=MH_ERROR_ALREADY_CREATED detour not installed (N126/N128)\n")
        self.assertEqual(by_name(checks.check_hooks(known), "known_hook_failure")[0].status, checks.WARN)
        no_reason = "[NEVR.PATCH] hook FAILED name=EchoVR::GetProcAddress target=0x1 reason=MH_ERROR_ACCESS_DENIED\n"
        self.assertEqual(by_name(checks.check_hooks(no_reason), "no_unexpected_hook_failure")[0].status, checks.FAIL)

    def test_scoped_headless_known_exception_requires_redundancy_status(self):
        scoped = ("[NEVR.PATCH] hook FAILED name=LoadLibraryW target=0x1 reason=MH_ERROR_ALREADY_CREATED "
                  "detour not installed (N126/N128)\n"
                  "[NEVR.PATCH] Server mode: headless\n"
                  "[NEVR.PATCH] Oculus Platform SDK blocking hooks: LoadLibraryW=FAILED LoadLibraryExW=FAILED "
                  "(redundant on headless - OVR SDK is never loaded; N127)\n")
        self.assertEqual(by_name(checks.check_hooks(scoped), "known_hook_failure")[0].status, checks.WARN)
        unscoped = scoped.replace("(redundant on headless - OVR SDK is never loaded; N127)", "(installation failed)")
        self.assertEqual(by_name(checks.check_hooks(unscoped), "no_unexpected_hook_failure")[0].status,
                         checks.FAIL)

    def test_unrecognized_hook_failure_format_fails_closed(self):
        results = checks.check_hooks("[NEVR.PATCH] unable to install hook target=0x1 status=FAILED\n")
        self.assertEqual(by_name(results, "no_unexpected_hook_failure")[0].status, checks.FAIL)

    def test_runtime_patch_emitter_formats_are_fail_closed(self):
        log = ("[NEVR.PATCH] hook failed name=CSysDLL_Load\n"
               "[NEVR.PATCH] hooks installed: 3 succeeded, 1 failed (failed: HTTPListenerBringup)\n")
        results = checks.check_hooks(log)
        self.assertEqual(by_name(results, "no_unexpected_hook_failure")[0].status, checks.FAIL)
        self.assertIn("CSysDLL_Load", by_name(results, "no_unexpected_hook_failure")[0].detail)

    def test_diag_hook_failure_is_a_scoped_warning(self):
        results = checks.check_hooks("[NEVR.PATCH] DIAG hook failed name=NetGameHostCheck va=0x1 reason=prologue_mismatch\n")
        self.assertEqual(by_name(results, "diagnostic_hook_failure")[0].status, checks.WARN)
        self.assertEqual(by_name(results, "no_unexpected_hook_failure")[0].status, checks.PASS)

    def test_required_hook_skip_is_failure(self):
        results = checks.check_hooks("[NEVR.PATCH] hook skipped name=CSysDLL_Load va=0x1 reason=prologue_mismatch\n")
        self.assertEqual(by_name(results, "no_unexpected_hook_failure")[0].status, checks.FAIL)

    def test_missing_all_hooks_installed_fails(self):
        self.assertEqual(by_name(checks.check_hooks("nothing useful\n"), "hooks_installed")[0].status,
                         checks.FAIL)

    def test_current_boot_hook_marker_accepts_boolean_case_variants(self):
        results = checks.check_hooks("[NEVR.PATCH] boot hooks installed ok=TrUe\n")
        self.assertEqual(by_name(results, "hooks_installed")[0].status, checks.PASS)

    def test_boot_hook_false_missing_and_malformed_values_fail(self):
        cases = (
            "[NEVR.PATCH] boot hooks installed ok=false\n",
            "[NEVR.PATCH] All other boot work completed\n",
            "[NEVR.PATCH] boot hooks installed ok=maybe\n",
            "[NEVR.PATCH] boot hooks installed\n",
        )
        for log in cases:
            with self.subTest(log=log):
                self.assertEqual(
                    by_name(checks.check_hooks(log), "hooks_installed")[0].status,
                    checks.FAIL,
                )

    def test_explicit_false_wins_over_true_boot_hook_marker(self):
        log = ("[NEVR.PATCH] boot hooks installed ok=true\n"
               "[NEVR.PATCH] boot hooks installed ok=FALSE\n")
        self.assertEqual(
            by_name(checks.check_hooks(log), "hooks_installed")[0].status,
            checks.FAIL,
        )

    def test_exact_known_hook_reasons_accept_reason_and_status_forms_in_headless_mode(self):
        log = ("[NEVR.HEADLESS] engine flags 0x1 -> 0x0 (bit0_render=CLEAR(HEADLESS))\n"
               "[NEVR.PATCH] hook FAILED name=LoadLibraryW target=0x1 reason=MH_ERROR_ALREADY_CREATED\n"
               "[NEVR.PATCH] hook failed name=LoadLibraryExW target=0x2 status=MH_ERROR_ALREADY_CREATED\n"
               "[NEVR.PATCH] boot hooks installed ok=true\n")
        results = checks.check_hooks(log)
        self.assertEqual(by_name(results, "no_unexpected_hook_failure")[0].status, checks.PASS)
        self.assertEqual(
            {result.detail.split(": ", 1)[0] for result in by_name(results, "known_hook_failure")},
            {"LoadLibraryW", "LoadLibraryExW"},
        )

    def test_unknown_or_mismatched_hook_failure_is_not_benign(self):
        cases = (
            "[NEVR.PATCH] hook FAILED name=NewHook reason=MH_ERROR_ALREADY_CREATED\n",
            "[NEVR.PATCH] hook failed name=LoadLibraryW status=MH_ERROR_UNSUPPORTED_FUNCTION\n",
            "[NEVR.PATCH] hook FAILED name=LoadLibraryW reason=MH_ERROR_ALREADY_CREATED\n",
        )
        for failure in cases:
            log = "[NEVR.PATCH] boot hooks installed ok=true\n" + failure
            with self.subTest(failure=failure):
                results = checks.check_hooks(log)
                self.assertEqual(
                    by_name(results, "no_unexpected_hook_failure")[0].status,
                    checks.FAIL,
                )

    def test_boot_success_does_not_mask_unexpected_hook_failure(self):
        log = ("[NEVR.PATCH] boot hooks installed ok=true\n"
               "[NEVR.PATCH] hook FAILED name=NewHook target=0x1 status=MH_ERROR_ALREADY_CREATED\n")
        results = checks.check_hooks(log)
        self.assertEqual(by_name(results, "hooks_installed")[0].status, checks.PASS)
        self.assertEqual(by_name(results, "no_unexpected_hook_failure")[0].status, checks.FAIL)
        self.assertFalse(checks.overall(results))


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

    def test_absent_config_json_still_completes_the_config_stage(self):
        # Issue #21: config.json is optional. The runtime's line for a search that
        # found none (verbatim from a Wine server run) ends the stage just like a load.
        log = ("Echo VR\n[NEVR.PATCH] no _local/config.json under "
               "Z:\\rig\\echovr\\bin\\win10\\ (optional — NEVR settings come from config.yaml)\n")
        self.assertEqual(checks.engine_stage_reached(log), "config_loaded")
        self.assertEqual(checks.check_engine_progress(log, "config_loaded").status, checks.PASS)

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
        self.assertEqual(checks.check_process_alive(False, None, "exit").status, checks.FAIL)
        self.assertEqual(checks.check_process_alive(True, None, "exit").status, checks.FAIL)

    def test_only_matching_run_id_can_supply_process_exit_state(self):
        stale = "run-old started\nrun-old exited rc=0"
        result, exit_code = checks.check_process_markers(stale, "run-new", False)
        self.assertEqual(result.status, checks.FAIL)
        self.assertIsNone(exit_code)
        result, exit_code = checks.check_process_markers("run-new started\nrun-new exited rc=7", "run-new", False)
        self.assertEqual(result.status, checks.PASS)
        self.assertEqual(exit_code, 7)

    def test_current_running_process_requires_its_own_start_marker(self):
        result, exit_code = checks.check_process_markers("run-old started", "run-new", True)
        self.assertEqual(result.status, checks.FAIL)
        self.assertIsNone(exit_code)
        result, exit_code = checks.check_process_markers("run-new started", "run-new", True)
        self.assertEqual(result.status, checks.PASS)
        self.assertIsNone(exit_code)

    def test_stale_missing_or_failed_enumeration_marker_is_not_a_pass(self):
        self.assertEqual(checks.check_window_enumeration("run-old completed", "run-new", "").status,
                         checks.FAIL)
        self.assertEqual(checks.check_window_enumeration(None, "run-new", "").status, checks.FAIL)
        self.assertEqual(checks.check_window_enumeration("run-new failed", "run-new", "").status,
                         checks.FAIL)

    def test_completed_empty_window_enumeration_is_valid(self):
        result = checks.check_window_enumeration("run-new completed", "run-new", "")
        self.assertEqual(result.status, checks.PASS)

    def test_window_enumeration_timeout_fails_only_while_game_is_live(self):
        self.assertEqual(checks.check_window_enumeration(None, "run-new", "", required=True).status,
                         checks.FAIL)
        self.assertEqual(checks.check_window_enumeration(None, "run-new", "", required=False).status,
                         checks.WARN)

    def test_window_dump_pid_selector_fixtures(self):
        selected = fixture("window_dump_pid_5572.txt")
        unrelated = fixture("window_dump_pid_6120.txt")
        self.assertEqual(checks.check_window_dump_pid(selected, 5572).status, checks.PASS)
        self.assertEqual(checks.check_window_dump_pid(unrelated, 5572).status, checks.FAIL)


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
