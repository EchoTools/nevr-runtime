"""#2410: the new-unlocks clear on Quest is a combat-area module over the shared rule header, built into the sentinel.

new_unlocks_clear_test.cpp drives the real wrappers through the two installed words, and got_pinned_test.cpp checks the two relocations and the
two code pins in the real libr15. This file pins what neither can see: that the decisions are the shared header's and are not copied into the Quest
file, that the game's SaveClientProfileChanges and the acknowledgement are not hooked, the combat target, the sentinel's install and budget row, the
host test's registration, the table the two builds share, and that the module changes no code page.

Python because every test in tools/tests is Python and the repository has no Go tools module yet; the file reads source text and runs nothing."""

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


QUEST = "src/quest/combat/new_unlocks_clear.cpp"


class TheRuleIsTheSharedHeaders(unittest.TestCase):
    def source(self):
        return strip_comments(read(QUEST))

    def test_the_quest_file_includes_the_shared_rule_header(self):
        self.assertIn('#include "runtime/patch/new_unlocks_clear_rules.h"', read(QUEST))

    def test_the_clear_wrapper_and_the_frame_work_hand_the_decision_to_the_rule(self):
        code = self.source()
        self.assertIn("g_hold.OnClear(", extract_braced_function(code, "void ClearHandler("))
        self.assertIn("g_hold.OnPoll(", extract_braced_function(code, "void PollNow() noexcept {"))

    def test_no_decision_is_copied_into_the_quest_file(self):
        code = self.source()
        for pattern in (r"\bclass\s+Hold\b", r"countBefore\s*==\s*0", r"\bkStillInFlight\s*=", r"countNow\s*!=\s*0\s*\)\s*\{\s*Drop"):
            self.assertIsNone(re.search(pattern, code), pattern)

    def test_the_original_runs_before_the_rule_is_asked(self):
        body = extract_braced_function(self.source(), "void ClearHandler(")
        self.assertEqual(body.count("original(netGame);"), 1)
        self.assertLess(body.index("original(netGame);"), body.index("g_hold.OnClear("))

    def test_the_game_save_and_the_acknowledgement_are_not_hooked(self):
        # libr15.so, libpnsovr.so and libpnsrad.so each define their own copies and call them through their own PLT slots; the poll sets the
        # dirty bit instead, a field every copy reads.
        code = self.source()
        self.assertNotIn("SaveClientProfileChanges", code)
        self.assertNotIn("ProfileUpdateSuccessCB", code)
        self.assertEqual(len(re.findall(r"InstallThunk<", code)), 1)  # one call site, run once per word

    def test_the_dirty_bit_is_set_atomically_after_the_user_is_rechecked(self):
        body = extract_braced_function(self.source(), "void PollNow() noexcept {")
        self.assertIn("__atomic_fetch_or(", body)
        self.assertLess(body.index("current != held"), body.index("__atomic_fetch_or("))

    def test_the_two_addresses_of_the_shared_table_are_the_pinned_ones(self):
        rules = strip_comments(read("src/runtime/patch/new_unlocks_clear_rules.h"))
        pinned = read("src/quest/sentinel/pinned_targets.h")
        self.assertRegex(rules, r"kQuestAddresses\s*=\s*\{0x1297114ULL,\s*0xFA82F8ULL\}")
        self.assertRegex(pinned, r"kNewUnlocksClear\s*=\s*\{\"_ZN10NRadEngine8NRadGame11CR15NetGame27RemoveAllNewlyUnlockedItemsEv\",\s*0x1297114ULL,")
        self.assertIn("kJsonEmptyVaddr = 0xfa82f8ULL;", pinned)
        self.assertIn("0x36ccac0ULL, 0x3709e08ULL", pinned)

    def test_the_lines_are_info_and_carry_the_user_and_the_count(self):
        code = self.source()
        body = extract_braced_function(code, "void LogEnd(")
        self.assertIn("sentinel::LogLevel::kInfo", body)
        for field in ('"msg"', '"user_id"', '"count"'):
            self.assertIn(field, body)
        for event in ("held", "rearmed", "superseded", "dropped"):
            self.assertIn(f'"combat_new_unlocks_clear_{event}"', code)


class TheModuleIsWired(unittest.TestCase):
    def test_the_combat_target_builds_it(self):
        block = re.search(r"add_library\(nevr_quest_combat STATIC([^)]*)\)", read("src/quest/combat/CMakeLists.txt"))
        self.assertIsNotNone(block)
        self.assertIn("new_unlocks_clear.cpp", block.group(1).split())

    def test_the_budget_has_its_two_handles(self):
        self.assertIn('{"combat/new_unlocks_clear.cpp", 2, 0}', read("src/quest/sentinel/hook_budget.h"))
        code = strip_comments(read(QUEST))
        self.assertRegex(code, r"constexpr std::size_t kHookCount = 2;\s*sentinel::GotHook g_hooks\[kHookCount\];")

    def test_every_launch_installs_it_after_the_equipment_re_fire(self):
        body = extract_braced_function(
            strip_comments(read("src/quest/sentinel/entry.cpp")), "bool InstallPermissionWaitHook() noexcept {"
        )
        self.assertEqual(body.count("nevr_quest::combat::new_unlocks_clear::Install();"), 1)
        self.assertLess(
            body.index("nevr_quest::combat::equip_refire::Install();"), body.index("nevr_quest::combat::new_unlocks_clear::Install();")
        )

    def test_the_host_test_is_a_target_of_the_combat_group(self):
        self.assertRegex(
            read("src/quest/host_tests/groups/combat.cmake"),
            r"nevr_ht_test\(new_unlocks_clear_test GROUP combat[^)]*src/quest/tests/new_unlocks_clear_test\.cpp"
            r"\s+src/quest/combat/new_unlocks_clear\.cpp\s+src/quest/invite/frame_dispatch\.cpp\)",
        )

    def test_the_pinned_test_resolves_the_words_and_the_code(self):
        text = read("src/quest/tests/got_pinned_test.cpp")
        self.assertIn("pinned::LibR15NewUnlocksClear(kind)", text)
        self.assertIn("pinned::kNewUnlocksClearCode[i]", text)

    def test_the_mutant_list_exists_for_the_runner(self):
        self.assertIn("runm nuc_original_skipped", read("src/quest/combat/mutants.d/new_unlocks_clear.sh"))

    def test_the_module_changes_no_code_page(self):
        # N151: the sentinel only stores data into libr15 (the GOT words through the hook manager).
        code = strip_comments(read(QUEST))
        for call in ("mprotect", "mmap", "mremap", "PROT_EXEC", "PROT_WRITE", "WriteWord", "InstallInlineThunk"):
            self.assertNotIn(call, code)

    def test_the_header_names_the_two_rules_it_does_not_follow(self):
        text = read("src/quest/combat/new_unlocks_clear.h")
        self.assertIn("ships to every APK", text)
        self.assertIn("not gated by combat::Active()", text)

    def test_the_logging_standard_lists_the_events(self):
        row = [line for line in read("docs/standards/logging.md").splitlines() if line.startswith("| `[NEVR.UNLOCKS]`")]
        self.assertEqual(len(row), 1)
        for event in ("combat_new_unlocks_clear_install", "combat_new_unlocks_clear_held", "combat_new_unlocks_clear_rearmed"):
            self.assertIn(event, row[0])


if __name__ == "__main__":
    unittest.main()
