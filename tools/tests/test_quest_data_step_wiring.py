"""#807: the first-run data step is wired into the game's start in the order the plan needs.

The hooks reach libr15's GOT, which no host test can. The host tests (data_wiring_test, permission_wait_hook_test)
drive the real handler and the step through the thunk entry; what they cannot see is that the production entry arms
the step, and the order inside the step that the game's start depends on. That is pinned here by source shape.
"""

import re
import unittest
from pathlib import Path

from tools.tests.test_runtime_lifecycle_invariants import extract_braced_function

ROOT = Path(__file__).resolve().parents[2]
SENTINEL = ROOT / "src/quest/sentinel"


def read(name):
    return (SENTINEL / name).read_text()


class TheProductionEntryArmsTheStep(unittest.TestCase):
    def test_the_data_step_is_armed_before_the_permission_wait_hook_is_installed(self):
        body = extract_braced_function(read("entry.cpp"), "bool InstallPermissionWaitHook() noexcept {")
        self.assertIn("data_wiring::Install(options)", body)
        self.assertIn("options.show = &nevr_quest::toast::Show", body)
        self.assertLess(body.index("data_wiring::Install(options)"), body.index("permission_wait::Install()"))


class TheHandlerRunsTheStepAfterTheWait(unittest.TestCase):
    def test_after_storage_runs_after_the_wait_and_only_for_the_boot_request(self):
        body = extract_braced_function(read("permission_wait_hook.cpp"), "void RequestHandler(")
        self.assertIn("g_spent.exchange(true", body)
        self.assertLess(body.index("g_spent.exchange(true"), body.index("wait(names, count)"))
        self.assertLess(body.index("wait(names, count)"), body.index("g_after.load"))
        self.assertLess(body.index("g_after.load"), body.index("after(outcome)"))

    def test_the_handler_reaches_the_step_only_through_a_pointer(self):
        body = extract_braced_function(read("permission_wait_hook.cpp"), "void RequestHandler(")
        self.assertNotIn("data_wiring", body)
        self.assertNotIn("AfterStorage(", body)


class TheAppIsKeptBeforeTheGameRuns(unittest.TestCase):
    def test_process_end_stores_the_app_before_the_original_android_main(self):
        body = extract_braced_function(read("process_end_hook.cpp"), "void MainHandler(")
        self.assertIn("g_app.store(app", body)
        self.assertLess(body.index("g_app.store(app"), body.index("original(app)"))


class TheStepOrder(unittest.TestCase):
    def body(self):
        return extract_braced_function(
            read("data_wiring.cpp"), "void AfterStorage(permission_wait::StorageOutcome outcome) noexcept {"
        )

    def test_the_root_is_resolved_again_only_after_a_fetch_and_before_the_game_continues(self):
        body = self.body()
        self.assertRegex(body, r"kFetched\)\s*obb_skip::ResolveRoot\(config\.exists\)")

    def test_a_failed_step_ends_nonzero_through_the_end_with_its_reason(self):
        body = self.body()
        self.assertRegex(body, r'if \(failed\) EndFailed\("data_step_failed"\)')
        self.assertLess(body.index("RunDataStep"), body.index("if (failed) EndFailed("))
        wiring = extract_braced_function(read("data_wiring.cpp"), "void EndFailed(const char* reason) noexcept {")
        self.assertIn("process_end::EndWithExitCode(process_end::kFailureExitCode, reason)", wiring)
        self.assertNotIn("EndProcess", re.sub(r"//[^\n]*", "", read("data_wiring.cpp")))
        end = extract_braced_function(
            read("process_end_hook.cpp"), "void EndWithExitCode(int exitCode, const char* reason) noexcept {"
        )
        self.assertNotIn("_exit(0)", end)

    def test_no_grant_skips_the_legacy_root_and_the_probe_but_not_the_data_step(self):
        body = self.body()
        self.assertIn("const bool granted = permission_wait::StorageGranted(outcome);", body)
        self.assertIn("if (granted && Opts().probe_enabled) RunProbe();", body)
        self.assertIn("config.legacy_root_readable = granted;", body)
        self.assertLess(body.index("config.legacy_root_readable = granted;"), body.index("RunDataStep"))
        # the how-to-allow-Storage message only when the media root could not be written and there is no grant
        self.assertRegex(body, r"failed && !granted && result\.status == data::FetchStatus::kBadRoot")
        gate = body.index("failed && !granted")
        self.assertIn("kNeedsStoragePermission", body[gate:])
        self.assertIn('EndFailed("no_storage_grant")', body[gate:])
        self.assertNotIn("StopWithoutGrant", re.sub(r"//[^\n]*", "", read("data_wiring.cpp")))

    def test_the_probe_runs_only_in_a_development_build_and_after_the_gate(self):
        body = self.body()
        self.assertRegex(body, r"if \(granted && Opts\(\)\.probe_enabled\) RunProbe\(\);")
        entry = extract_braced_function(read("entry.cpp"), "bool InstallPermissionWaitHook() noexcept {")
        self.assertIn("options.probe_enabled = nevr_build::kIsDev;", entry)

    def test_there_is_no_cancel_while_the_step_blocks_android_main(self):
        code = re.sub(r"//[^\n]*", "", read("data_wiring.cpp"))
        self.assertNotIn("keep_going", code)
        self.assertNotIn("NotDestroyed", code)
        self.assertNotIn("kAppDestroyRequested", code)
        self.assertNotIn("config.fetch.backoff", code)  # the fetch's default backoff applies

    def test_the_download_bounds(self):
        header = read("data_wiring.h")
        self.assertRegex(header, r"kStallSeconds\s*=\s*120;")
        self.assertRegex(header, r"kMinBytesPerSecond\s*=\s*16 \* 1024;")
        wiring = extract_braced_function(
            read("data_wiring.cpp"), "std::unique_ptr<data::CurlTransport> MakeProductionTransport() {"
        )
        self.assertIn("kStallSeconds", wiring)
        self.assertIn("kMinBytesPerSecond", wiring)

    def test_the_message_is_held_ten_seconds(self):
        self.assertRegex(read("data_wiring.h"), r"unsigned hold_ms = 5000;")
        hold = extract_braced_function(read("data_wiring.cpp"), "void HoldMessage(const std::string& message) noexcept {")
        self.assertEqual(hold.count("Sleep(Opts().hold_ms)"), 2)

    def test_the_log_never_says_shown_for_a_toast_that_was_only_requested(self):
        text = re.sub(r"//[^\n]*", "", read("data_wiring.cpp") + read("toast_presenter.cpp"))
        self.assertIn('"requested"', text)
        self.assertNotIn('"shown"', text)

    def test_the_probe_is_consumed_before_it_acts(self):
        body = extract_braced_function(read("data_wiring.cpp"), "void RunProbe() noexcept {")
        self.assertLess(body.index("std::rename"), body.index('"toast "'))
        self.assertLess(body.index("std::rename"), body.index('"block "'))
        self.assertIn("kMaxProbeBlockSeconds", body)


class TheLeftoversArePutRightBeforeTheDecision(unittest.TestCase):
    def test_recover_runs_before_clean_and_before_resolve_on_every_root(self):
        data = ROOT / "src/quest/data/data_step.cpp"
        text = data.read_text()
        put = extract_braced_function(text, "void PutLeftoversRight(const StepConfig& config) {")
        self.assertLess(put.index("RecoverReplaced("), put.index("CleanLeftovers("))
        self.assertIn("RecoveryRootsFor(config)", put)
        roots = extract_braced_function(text, "std::vector<std::string> RecoveryRootsFor(const StepConfig& config) {")
        self.assertIn("data_root::kCandidates", roots)
        self.assertIn("data_root::kLegacyRoot", roots)  # dropped without the grant
        run = extract_braced_function(text, "StepResult RunDataStep(const StepConfig& config, Transport& transport) {")
        self.assertLess(run.index("PutLeftoversRight(config)"), run.index("data_root::Resolve("))

    def test_the_notice_comes_from_the_fetch_after_its_space_check(self):
        fetch = (ROOT / "src/quest/data/fetch.cpp").read_text()
        self.assertIn("c.before_download", fetch)
        self.assertLess(fetch.index("kNoSpace"), fetch.index("c.before_download"))


class TheToastClearsEveryException(unittest.TestCase):
    def test_pending_is_checked_before_the_result_after_every_call(self):
        text = re.sub(r"//[^\n]*", "", read("toast_presenter.cpp"))
        self.assertNotRegex(text, r"if \([a-zA-Z]+ == nullptr \|\| Pending\(env\)\)")
        self.assertIn("ExceptionClear", text)


class NoLoadTimeInitializer(unittest.TestCase):
    def test_the_wiring_holds_no_global_with_a_constructor(self):
        text = re.sub(r"//[^\n]*", "", read("data_wiring.cpp"))
        self.assertNotRegex(text, r"^(?:static\s+)?Options\s+\w+\s*;", "a global Options is a dynamic initializer")
        self.assertIn("static Options options;", text)  # the function-local one the step uses


class NoUrlInTheLogAndNoCatchAll(unittest.TestCase):
    FILES = ["data_wiring.cpp", "toast_presenter.cpp"]

    def test_no_url_literal_in_the_wiring(self):
        for name in self.FILES:
            text = re.sub(r"//[^\n]*", "", read(name))
            self.assertNotIn("https://", text, name)
            self.assertNotIn("kZipUrl", text, name)

    def test_no_catch_all(self):
        for name in self.FILES + ["android_glue_layout_check.cpp"]:
            self.assertIsNone(re.search(r"catch\s*\(\s*\.\.\.\s*\)", read(name)), name)


class TheGlueOffsetsAreAssertedAgainstTheNdkHeaders(unittest.TestCase):
    CONSTANTS = {
        "kAppActivity": "0x18",
        "kAppDestroyRequested": "0x64",
        "kActivityVm": "0x8",
        "kActivityEnv": "0x10",
        "kActivityClazz": "0x18",
    }

    def test_every_constant_has_its_value_and_a_static_assert(self):
        layout = read("android_glue_layout.h")
        check = read("android_glue_layout_check.cpp")
        for name, value in self.CONSTANTS.items():
            self.assertRegex(layout, rf"{name}\s*=\s*{value};", name)
            self.assertIn(f"sentinel::glue::{name}", check, name)
        self.assertEqual(len(re.findall(r"^static_assert\(", check, re.M)), len(self.CONSTANTS) + 1)  # + the JavaVM slot

    def test_the_javavm_slot_the_game_calls_is_asserted_by_name(self):
        check = read("android_glue_layout_check.cpp")
        self.assertIn("offsetof(JNIInvokeInterface, DetachCurrentThread) == 0x28", check)


if __name__ == "__main__":
    unittest.main()
