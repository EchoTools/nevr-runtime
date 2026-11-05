"""#1986: the controller type and raw pose log is built into the sentinel, installed on every launch, and reads; the one write
is the interim Controller Hand Flip roll (KNOWN-WRONG #2043), behind the toggle.

The handlers are driven by hand_pose_log_hook_test.cpp and the two slots are checked against the real libr15 by
got_pinned_test.cpp. This file pins what neither can see: the build and the install, the pinned slots, that the file hooks no
other slot, that the only write to a pose is the roll (sixteen bytes, the orientation, after the raw pose is taken and only for
a table device while the toggle is on), and that every site of the interim carries the marker that names #2043.
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
        hooks_cmake = read("src/quest/host_tests/groups/hooks.cmake")
        self.assertRegex(hooks_cmake, r"nevr_ht_test\(hand_pose_log_hook_test GROUP hooks[^)]*TIMEOUT 120[^)]*src/quest/tests/hand_pose_log_hook_test\.cpp")
        self.assertRegex(hooks_cmake, r"nevr_ht_test\(hand_flip_test GROUP hooks[^)]*TIMEOUT 120[^)]*src/quest/tests/hand_flip_test\.cpp")


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
        # Nothing is written through the pointers the runtime was given, by these two functions: the one write is RollOrientation's.
        for body in (enumerate, tracking):
            self.assertNotRegex(body, r"memcpy\(\s*(header|tracking)\s*,")
            self.assertNotRegex(body, r"\*\s*\(?(header|tracking)\)?\s*=")

    def test_the_handlers_do_not_log(self):
        code = self.source()
        for name in ("int EnumerateHandler(", "int TrackingHandler("):
            body = extract_braced_function(code, name)
            self.assertNotIn("LogFields", body)
            self.assertNotIn("LogEvent", body)


class TheInterimRollIsTheOnlyWriteAndIsMarked(unittest.TestCase):
    """KNOWN-WRONG #2043: the roll, its toggle, its row, its file and its lines are the interim (ADR 0033 rule 3)."""

    def test_the_roll_runs_after_the_raw_pose_is_taken_for_a_table_device_with_the_toggle_on(self):
        code = strip_comments(read("src/quest/sentinel/hand_pose_log_hook.cpp"))
        tracking = extract_braced_function(code, "int TrackingHandler(")
        order = [tracking.index(name) for name in ("Find(deviceId)", "RecordFirstPose(slot, tracking)", "hand_flip::Enabled()", "RollOrientation(tracking)")]
        self.assertEqual(order, sorted(order))
        self.assertRegex(tracking, r"if \(slot == nullptr\) return rc;")
        self.assertRegex(tracking, r"if \(hand_flip::Started\(\) && hand_flip::Enabled\(\)\) RollOrientation\(tracking\);")
        roll = extract_braced_function(code, "void RollOrientation(")
        self.assertEqual(len(re.findall(r"memcpy\(", roll)), 3)  # the status read, the orientation read, the one write
        self.assertEqual(len(re.findall(r"memcpy\(\s*static_cast<unsigned char\*>\(tracking\)", roll)), 1)  # the write
        self.assertIn("offsetof(TrackingPrefix, orientation)", roll)
        self.assertIn("sizeof(orientation)", roll)
        self.assertEqual(code.count("RollOrientation("), 2)  # its definition and its one call

    def test_the_handler_does_not_log_or_allocate_for_the_roll(self):
        code = strip_comments(read("src/quest/sentinel/hand_pose_log_hook.cpp"))
        for name in ("int TrackingHandler(", "void RollOrientation("):
            body = extract_braced_function(code, name)
            for forbidden in ("LogFields", "LogEvent", "new ", "std::string", "std::vector"):
                self.assertNotIn(forbidden, body)

    def test_the_row_is_bound_to_the_carrier_and_the_delivery_reads_it(self):
        header = strip_comments(read("src/quest/sentinel/hand_flip.h"))
        self.assertIn('kMenuPath = "graphics|fullscreen"', header)
        self.assertIn("kCarrierOffset = 0x00", header)
        self.assertIn("kCarrierBit = 1U", header)
        hook = strip_comments(read("src/quest/sentinel/texture_menu_hook.cpp"))
        apply = extract_braced_function(hook, "void HandleApply(")
        self.assertIn("HandleHandFlipDelivery(const_cast<unsigned char*>(settings));", apply)
        delivery = extract_braced_function(hook, "void HandleHandFlipDelivery(")
        # The game's own value of the bit is 1 and that is Off: a change reads as the inverse of the bit, and the load writes the
        # inverse of the choice, so a player who never turns the toggle on leaves the bit as the game holds it.
        self.assertIn("hand_flip::SetFromMenu(hand_flip::FlipFor(carried))", delivery)
        self.assertIn("hand_flip::BitFor(hand_flip::Current())", delivery)
        self.assertIn("hand_flip::RecordCarrier(before, carried)", delivery)
        self.assertIn("hand_flip::kCarrierBit", delivery)
        self.assertRegex(header, r"kMenuLabels\[kMenuValueCount\] = \{\"On\", \"Off\"\}")
        self.assertRegex(header, r"BitFor\(int flip\) noexcept \{ return flip != 0 \? 0 : 1; \}")
        rewrite = extract_braced_function(hook, "Served RewriteDocument(")
        self.assertIn("hand_flip::MenuIndexFor(", rewrite)
        # The kept rewrite is keyed on the row's index as well as the text, so a re-read serves the current choice.
        self.assertIn("kept.handFlipIndex == handFlipIndex", rewrite)
        self.assertIn("Kept{hash, input.size(), handFlipIndex, out}", rewrite)

    def test_every_site_of_the_interim_names_the_issue_that_tracks_it(self):
        sites = (
            "src/quest/sentinel/hand_flip.h",
            "src/quest/sentinel/hand_pose_log_hook.cpp",
            "src/quest/sentinel/hand_pose_log_hook.h",
            "src/quest/sentinel/texture_menu.cpp",
            "src/quest/sentinel/texture_menu.h",
            "src/quest/sentinel/texture_menu_hook.cpp",
            "src/quest/sentinel/entry.cpp",
            "src/quest/tests/hand_flip_test.cpp",
            "docs/design/2026-11-02-quest-hand-flip-toggle.md",
        )
        for path in sites:
            text = read(path)
            self.assertIn("KNOWN-WRONG #2043", text, path)
            # The report these markers once named is closed by the change that built the interim; a marker names the open issue.
            self.assertNotIn("KNOWN-" + "WRONG #1986", text, path)  # two pieces: tools/check_known_wrong.py reads a literal marker as live

    def test_the_row_is_offered_only_when_both_hooks_and_the_poller_are_in(self):
        code = strip_comments(read("src/quest/sentinel/hand_pose_log_hook.cpp"))
        install = extract_braced_function(code, "bool InstallAt(")
        self.assertIn("const bool polling = e == sentinel::GotStatus::kOk && StartPoller();", install)
        self.assertIn("hand_flip::SetOffered(all && polling);", install)
        hook = strip_comments(read("src/quest/sentinel/texture_menu_hook.cpp"))
        rewrite = extract_braced_function(hook, "Served RewriteDocument(")
        self.assertIn("hand_flip::Offered()", rewrite)
        self.assertIn("texture_menu::kNoHandFlipRow", rewrite)
        self.assertIn("texture_menu::RewriteGameSettings(input, offered)", rewrite)
        delivery = extract_braced_function(hook, "void HandleHandFlipDelivery(")
        self.assertRegex(delivery, r"if \(!hand_flip::Offered\(\)\) return;")
        # The start is awaited to completion (a condition variable, with a timeout) before the flag is trusted.
        self.assertRegex(delivery, r"if \(!hand_flip::AwaitStart\(\)\) return;")
        self.assertLess(delivery.index("hand_flip::AwaitStart()"), delivery.index("hand_flip::Current()"))
        header = strip_comments(read("src/quest/sentinel/hand_flip.h"))
        await_start = extract_braced_function(header, "inline bool AwaitStart() noexcept {")
        self.assertIn("pthread_cond_timedwait(&g_startCond, &g_startMutex, &deadline)", await_start)
        self.assertNotRegex(await_start, r"\bwhile \(!?Started\(\)\)")  # no spin on the state
        self.assertIn("Offered()", extract_braced_function(header, "inline void StartFromDirectory(const char* directory) noexcept {"))

    def test_the_per_frame_path_never_waits_and_rolls_only_a_started_tracked_pose(self):
        code = strip_comments(read("src/quest/sentinel/hand_pose_log_hook.cpp"))
        tracking = extract_braced_function(code, "int TrackingHandler(")
        self.assertNotIn("AwaitStart", tracking)
        self.assertRegex(tracking, r"if \(hand_flip::Started\(\) && hand_flip::Enabled\(\)\) RollOrientation\(tracking\);")
        roll = extract_braced_function(code, "void RollOrientation(")
        self.assertIn("(prefix.status & kStatusOrientationTracked) == 0", roll)

    def test_a_save_that_fails_is_a_warning_and_is_retried(self):
        header = strip_comments(read("src/quest/sentinel/hand_flip.h"))
        write = extract_braced_function(header, "inline int WritePending() noexcept {")
        self.assertNotIn("kError", write)
        self.assertEqual(write.count("sentinel::LogLevel::kWarn"), 2)
        self.assertIn("g_pendingSave.compare_exchange_strong(none, flip", write)

    def test_nothing_else_in_the_sentinel_writes_a_tracking_pose(self):
        sentinel = ROOT / "src/quest/sentinel"
        for path in sorted(sentinel.glob("*.cpp")):
            if path.name == "hand_pose_log_hook.cpp":
                continue
            self.assertNotIn("hand_flip::Roll", strip_comments(path.read_text()), path.name)


if __name__ == "__main__":
    unittest.main()
