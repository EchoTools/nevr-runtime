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
        self.assertLess(body.index("g_after.load"), body.index("after()"))

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
        return extract_braced_function(read("data_wiring.cpp"), "void AfterStorage() noexcept {")

    def test_the_root_is_resolved_again_only_after_a_fetch_and_before_the_game_continues(self):
        body = self.body()
        self.assertRegex(body, r"kFetched\)\s*obb_skip::ResolveRoot\(config\.exists\)")

    def test_a_failed_step_ends_the_process(self):
        body = self.body()
        self.assertRegex(body, r"if \(failed\) EndProcess\(\)")
        self.assertLess(body.index("RunDataStep"), body.index("if (failed) EndProcess()"))

    def test_the_probe_runs_first(self):
        body = self.body()
        self.assertTrue(body.lstrip("{ \n").startswith("RunProbe();"))

    def test_the_probe_is_consumed_before_it_acts(self):
        body = extract_braced_function(read("data_wiring.cpp"), "void RunProbe() noexcept {")
        self.assertLess(body.index("std::rename"), body.index('"toast "'))
        self.assertLess(body.index("std::rename"), body.index('"block "'))
        self.assertIn("kMaxProbeBlockSeconds", body)


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
