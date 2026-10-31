"""#1759: a free that would overflow libr15's array of free GPU ranges is not run.

The handler is driven by vk_free_guard_test.cpp, and the slot, the prologue and the words the layout rests on are checked
against the real libr15 by got_pinned_test.cpp. This file pins what neither can see: that production builds the hook
without exceptions, installs it on every launch, runs its host test, and that the layout constants agree with the words
they were read from.
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


class TheGuardIsWired(unittest.TestCase):
    def test_the_slot_is_the_games_own_function(self):
        text = read("src/quest/sentinel/pinned_targets.h")
        self.assertIn('kVkFreeSymbol = "_ZN10NRadEngine16CGRawInterfaceVK4FreeEP16VkDeviceMemory_Tmmm"', text)
        body = extract_braced_function(text, "inline GotTarget LibR15VkFree() {")
        self.assertRegex(body, r"kLibR15,\s*kVkFreeSymbol,\s*RelocKind::kJumpSlot,\s*LibR15PatchedBuildId\(\),\s*0x37008d8ULL")

    def test_the_hook_is_built_without_exceptions_into_the_sentinel(self):
        cmake = read("src/quest/sentinel/CMakeLists.txt")
        self.assertRegex(cmake, r"add_library\(ovrplatformloader SHARED[^)]*\bvk_free_guard\.cpp")
        props = re.search(r"set_source_files_properties\(([^)]*)PROPERTIES COMPILE_OPTIONS \"-fno-exceptions\"\)", cmake)
        self.assertIsNotNone(props)
        self.assertIn("vk_free_guard.cpp", props.group(1))

    def test_every_launch_installs_it(self):
        body = extract_braced_function(strip_comments(read("src/quest/sentinel/entry.cpp")), "bool InstallVkExtWindowHook() noexcept {")
        self.assertIn("nevr_quest::vk_free_guard::Install();", body)

    def test_the_host_test_runs_in_the_hooks_recipe(self):
        justfile = read("justfile")
        self.assertIn("src/quest/tests/vk_free_guard_test.cpp", justfile)
        self.assertIn('timeout 120 "$out/vk_free_guard_test"', justfile)

    def test_the_real_library_check_covers_the_slot_and_the_layout(self):
        text = read("src/quest/tests/got_pinned_test.cpp")
        self.assertIn("pinned::LibR15VkFree()", text)
        self.assertIn("&pinned::kVkFree}", text)
        for block in ("kVkFreeCountBlock", "kVkFreeAppendBlock", "kVkFreeAppendStore"):
            self.assertIn(f"&pinned::{block}", text)
        self.assertIn("NobitsHolds(r15, pinned::vk_hole_layout::kTableVaddr", text)


class TheLayoutIsTheWordsTheyWereReadFrom(unittest.TestCase):
    def test_the_capacity_is_the_distance_from_the_table_to_the_count(self):
        pins = read("src/quest/sentinel/pinned_targets.h")
        table = int(re.search(r"kTableVaddr = (0x[0-9a-f]+)ULL", pins).group(1), 16)
        count = int(re.search(r"kCountVaddr = (0x[0-9a-f]+)ULL", pins).group(1), 16)
        stride = int(re.search(r"kEntryBytes = (0x[0-9a-f]+);", pins).group(1), 16)
        capacity = int(re.search(r"vk_hole_layout \{.*?kCapacity = (\d+);", pins, re.S).group(1))
        self.assertEqual((count - table) // stride, capacity)
        self.assertEqual((count - table) % stride, 0)
        table_header = read("src/quest/sentinel/vk_hole_table.h")
        self.assertIn(f"kCapacity = {capacity};", table_header)

    def test_the_pinned_append_block_stores_the_count_at_the_end_of_the_array(self):
        pins = read("src/quest/sentinel/pinned_targets.h")
        words = re.search(r"kVkFreeAppendBlock = \{[^{]*\{([^}]*)\}", pins).group(1)
        mov_w14 = [int(w, 16) for w in re.findall(r"0x[0-9a-f]+", words)][5]
        # movz w14, #imm16: bits 20:5 hold the immediate; the register is the low five bits.
        self.assertEqual(mov_w14 & 0x1F, 14)
        self.assertEqual((mov_w14 >> 5) & 0xFFFF, 0x5000)
        self.assertEqual(0x5000, 512 * 0x28)


if __name__ == "__main__":
    unittest.main()
