"""#1986: the controller type and raw pose log is built into the sentinel, installed on every launch, and only reads.

The handlers are driven by hand_pose_log_hook_test.cpp and the two slots are checked against the real libr15 by
got_pinned_test.cpp. This file pins what neither can see: the build and the install, the pinned slots, and that the file
hooks no other slot and writes nothing to a pose.
"""

import re
import unittest
from pathlib import Path

from tools.tests.test_runtime_lifecycle_invariants import extract_braced_function

ROOT = Path(__file__).resolve().parents[2]


def read(path: str) -> str:
    return (ROOT / path).read_text()


def strip_comments(text: str) -> str:
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    return re.sub(r"//[^\n]*", "", text)


class TheHandPoseLogIsWired(unittest.TestCase):
    def test_the_two_slots_are_the_games_own_vrapi_imports(self):
        text = read("src/quest/sentinel/pinned_targets.h")
        self.assertIn('kEnumerateInputDevicesSymbol = "vrapi_EnumerateInputDevices"', text)
        self.assertIn('kGetInputTrackingStateSymbol = "vrapi_GetInputTrackingState"', text)
        enumerate = extract_braced_function(text, "inline GotTarget LibR15EnumerateInputDevices() {")
        self.assertRegex(
            enumerate, r"kLibR15,\s*kEnumerateInputDevicesSymbol,\s*RelocKind::kJumpSlot,\s*LibR15PatchedBuildId\(\),\s*0x36d5dd0ULL"
        )
        tracking = extract_braced_function(text, "inline GotTarget LibR15GetInputTrackingState() {")
        self.assertRegex(
            tracking, r"kLibR15,\s*kGetInputTrackingStateSymbol,\s*RelocKind::kJumpSlot,\s*LibR15PatchedBuildId\(\),\s*0x36dc280ULL"
        )

    def test_the_pinned_test_resolves_both_slots(self):
        text = read("src/quest/tests/got_pinned_test.cpp")
        self.assertIn("pinned::LibR15EnumerateInputDevices()", text)
        self.assertIn("pinned::LibR15GetInputTrackingState()", text)

    def test_it_is_built_without_exceptions_into_the_sentinel(self):
        cmake = read("src/quest/sentinel/CMakeLists.txt")
        self.assertRegex(cmake, r"add_library\(ovrplatformloader SHARED[^)]*\bhand_pose_log_hook\.cpp")
        props = re.search(r"set_source_files_properties\(([^)]*)PROPERTIES COMPILE_OPTIONS \"-fno-exceptions\"\)", cmake)
        self.assertIsNotNone(props)
        self.assertIn("hand_pose_log_hook.cpp", props.group(1))

    def test_every_launch_installs_it(self):
        body = extract_braced_function(
            strip_comments(read("src/quest/sentinel/entry.cpp")), "bool InstallPermissionWaitHook() noexcept {"
        )
        self.assertEqual(body.count("nevr_quest::hand_pose_log::Install();"), 1)

    def test_the_host_test_runs_in_the_hooks_recipe(self):
        justfile = read("justfile")
        self.assertIn("src/quest/tests/hand_pose_log_hook_test.cpp", justfile)
        self.assertIn('timeout 120 "$out/hand_pose_log_hook_test"', justfile)


class TheHandPoseLogOnlyReads(unittest.TestCase):
    def source(self):
        return strip_comments(read("src/quest/sentinel/hand_pose_log_hook.cpp"))

    def test_it_installs_two_hooks_and_none_on_the_capabilities_slot(self):
        code = self.source()
        self.assertEqual(len(re.findall(r"InstallThunk<", code)), 2)
        self.assertIn("InstallThunk<EnumerateThunk>(g_enumerateHook, enumerate)", code)
        self.assertIn("InstallThunk<TrackingThunk>(g_trackingHook, tracking)", code)
        # The capabilities slot is read (the game's own function pointer) and never written, so the input seam, which reads
        # it the same way, meets the same function whatever the order of the two.
        self.assertNotIn("InstallThunk<Capabilities", code)
        self.assertEqual(code.count("LibR15GetInputDeviceCapabilities()"), 1)

    def test_the_handlers_call_the_original_first_and_return_its_result(self):
        code = self.source()
        enumerate = extract_braced_function(code, "int EnumerateHandler(")
        tracking = extract_braced_function(code, "int TrackingHandler(")
        self.assertRegex(enumerate, r"const int rc = original\(ovr, index, header\);")
        self.assertRegex(tracking, r"const int rc = original\(ovr, deviceId, absTime, tracking\);")
        # Nothing is written through the pointers the runtime was given.
        for body in (enumerate, tracking):
            self.assertNotRegex(body, r"memcpy\(\s*(header|tracking)\s*,")
            self.assertNotRegex(body, r"\*\s*\(?(header|tracking)\)?\s*=")

    def test_the_handlers_do_not_log(self):
        code = self.source()
        for name in ("int EnumerateHandler(", "int TrackingHandler("):
            body = extract_braced_function(code, name)
            self.assertNotIn("LogFields", body)
            self.assertNotIn("LogEvent", body)


if __name__ == "__main__":
    unittest.main()
