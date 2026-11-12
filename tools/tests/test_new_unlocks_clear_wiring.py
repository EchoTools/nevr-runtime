"""The PC new-unlocks clear hooks (#2410, src/runtime/patch/new_unlocks_clear.cpp): the parts a host test cannot reach without the game.

When a clear is held and when the dirty bit is set again is one definition in src/runtime/patch/new_unlocks_clear_rules.h, driven by
src/runtime/tests/test_new_unlocks_clear.cpp. What stays here is the glue's wiring, read as source: the hooked clear and the
emptiness test are pinned by bytes at the addresses the rules header lists, the hooks go through that table and the rules, the runtime
installs them once at boot, and the scenario control calls the clear through the detour.

Python because every test in tools/tests is Python and the repository has no Go tools module yet; the file reads source text and runs
nothing."""

import re
import unittest
from pathlib import Path

from tools.tests.test_runtime_lifecycle_invariants import strip_comments

ROOT = Path(__file__).resolve().parents[2]
GLUE = ROOT / "src/runtime/patch/new_unlocks_clear.cpp"
RULES = ROOT / "src/runtime/patch/new_unlocks_clear_rules.h"


def read(path):
    return strip_comments(path.read_text())


class Addresses(unittest.TestCase):
    def test_the_rules_table_names_the_pc_functions(self):
        text = read(RULES)
        self.assertRegex(text, r"kPcAddresses\s*=\s*\{0x1401A06C0ULL,\s*0x1405F1360ULL\}")
        self.assertRegex(text, r"kQuestAddresses\s*=\s*\{0x1297114ULL,\s*0xFA82F8ULL\}")

    def test_the_glue_pins_each_function_by_bytes_before_hooking(self):
        text = read(GLUE)
        for name in ("kClearPrologue", "kJsonEmptyPrologue"):
            self.assertIn(name, text)
        for va in ("a.clear", "a.jsonEmpty"):
            self.assertIn(f"Matches(gameBase, {va}", text)
        self.assertLess(text.index("Matches(gameBase, a.clear"), text.index("PatchDetour(&g_clearOriginal"))

    def test_the_prologue_of_the_clear_is_the_one_the_scenario_control_pins(self):
        glue = read(GLUE)
        control = read(ROOT / "src/runtime/scenario/scenario_control.cpp")
        pattern = r"\{([^}]*)\}"
        glue_bytes = re.findall(r"0x[0-9A-Fa-f]{2}", re.search(r"kClearPrologue\s*=\s*" + pattern, glue).group(1))
        control_bytes = re.findall(r"0x[0-9A-Fa-f]{2}", re.search(r"kClearNewUnlocksPrologue\s*=\s*" + pattern, control).group(1))
        self.assertEqual([b.lower() for b in glue_bytes], [b.lower() for b in control_bytes])


class Hooks(unittest.TestCase):
    def test_the_clear_is_detoured_and_the_decisions_are_the_rules(self):
        text = read(GLUE)
        self.assertIn('PatchDetour(&g_clearOriginal, reinterpret_cast<void*>(&ClearHook), "ClearNewUnlocks")', text)
        self.assertIn("g_hold.OnClear(", text)
        self.assertIn("g_hold.OnPoll(", text)
        for logic in ("countBefore == 0", "kStillInFlight ="):
            self.assertNotIn(logic, text, f"{logic} belongs to new_unlocks_clear_rules.h")

    def test_the_game_save_is_not_hooked(self):
        # The live SaveClientProfileChanges is the platform module's, not echovr.exe's: a detour on the echovr.exe copy is never called in a client run.
        text = read(GLUE)
        self.assertNotIn("SaveClientProfileChanges", text)
        self.assertEqual(text.count("PatchDetour("), 1)

    def test_the_original_runs_in_the_clear_hook(self):
        self.assertEqual(read(GLUE).count("g_clearOriginal(netGame)"), 1)

    def test_the_dirty_bit_is_set_with_the_rules_constants_after_the_user_is_rechecked(self):
        text = read(GLUE)
        self.assertIn("InterlockedOr(reinterpret_cast<volatile LONG*>(Bytes(held) + kUserFlagsOffset), static_cast<LONG>(kUserDirtyBit))", text)
        self.assertLess(text.index("reinterpret_cast<void*>(current) != held"), text.index("InterlockedOr(reinterpret_cast<volatile LONG*>"))
        self.assertIn("g_hold.Drop()", text)

    def test_the_lines_are_info_with_the_user_and_the_count(self):
        text = read(GLUE)
        lines = re.findall(r'Log\(EchoVR::LogLevel::(\w+), "\[NEVR\.UNLOCKS\] %s user_id=%llu count=%llu"', text)
        self.assertEqual(lines, ["Info", "Info", "Info", "Info"])  # held, dropped, superseded, re-armed
        for message in ("kMsgHeld", "kMsgDropped", "kMsgSuperseded", "kMsgRearmed"):
            self.assertIn(message, text)
        self.assertNotIn("DIAG", text)


class Wiring(unittest.TestCase):
    def test_installed_once_at_boot(self):
        text = read(ROOT / "src/runtime/lifecycle/initialize.cpp")
        self.assertEqual(text.count("nevr_new_unlocks_clear::Install("), 1)

    def test_polled_once_per_frame_on_the_game_loop_thread(self):
        text = read(ROOT / "src/runtime/frame/tick.cpp")
        self.assertIn("loop == GetCurrentThreadId()) nevr_new_unlocks_clear::OnFrame();", text)
        self.assertEqual(text.count("nevr_new_unlocks_clear::OnFrame()"), 1)

    def test_registered_in_the_build_and_the_inventory(self):
        self.assertIn('"patch/new_unlocks_clear.cpp"', (ROOT / "src/runtime/CMakeLists.txt").read_text())
        self.assertIn("add_executable(test_new_unlocks_clear tests/test_new_unlocks_clear.cpp)", (ROOT / "src/runtime/CMakeLists.txt").read_text())
        self.assertIn('"patch/new_unlocks_clear.cpp"', (ROOT / "tools/verify_patch_source_inventory.py").read_text())
        self.assertIn("test_new_unlocks_clear.exe", (ROOT / "justfile").read_text())

    def test_the_scenario_control_calls_the_clear_through_the_detour(self):
        text = read(ROOT / "src/runtime/scenario/scenario_control.cpp")
        self.assertIn("IsOurDetour(clearTarget)", text)

    def test_the_tag_is_in_the_logging_standard(self):
        self.assertIn("`[NEVR.UNLOCKS]`", (ROOT / "docs/standards/logging.md").read_text())


if __name__ == "__main__":
    unittest.main()
