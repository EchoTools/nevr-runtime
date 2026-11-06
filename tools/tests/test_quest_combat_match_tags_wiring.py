"""#2090: combat name tags are on by default through a CR15Game constructor hook, built into the sentinel and installed on every launch.

The handlers are driven by match_tag_default_test.cpp and the two slots are checked against the real libr15 by
got_pinned_test.cpp. This file pins what neither can see: the new combat target and its flags, the sentinel's link and install,
the budget row, the pinned slots, and that the file hooks only the two constructor slots and writes only the two flags words.
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


class TheMatchTagsHookIsWired(unittest.TestCase):
    def test_the_two_slots_are_the_constructors_own_jump_slots(self):
        text = read("src/quest/sentinel/pinned_targets.h")
        self.assertIn('kCR15GameC1Symbol = "_ZN10NRadEngine8NRadGame8CR15GameC1ERKNS_9CMainArgsE"', text)
        self.assertIn('kCR15GameC2Symbol = "_ZN10NRadEngine8NRadGame8CR15GameC2ERKNS_9CMainArgsE"', text)
        c1 = extract_braced_function(text, "inline GotTarget LibR15CR15GameC1() {")
        self.assertRegex(c1, r"kLibR15,\s*kCR15GameC1Symbol,\s*RelocKind::kJumpSlot,\s*LibR15PatchedBuildId\(\),\s*0x36c0018ULL")
        c2 = extract_braced_function(text, "inline GotTarget LibR15CR15GameC2() {")
        self.assertRegex(c2, r"kLibR15,\s*kCR15GameC2Symbol,\s*RelocKind::kJumpSlot,\s*LibR15PatchedBuildId\(\),\s*0x36e7828ULL")

    def test_the_pinned_test_resolves_both_slots(self):
        text = read("src/quest/tests/got_pinned_test.cpp")
        self.assertIn("pinned::LibR15CR15GameC1()", text)
        self.assertIn("pinned::LibR15CR15GameC2()", text)

    def test_the_combat_target_is_built_without_exceptions_and_hidden(self):
        cmake = read("src/quest/combat/CMakeLists.txt")
        library = re.search(r"add_library\(nevr_quest_combat STATIC([^)]*)\)", cmake)
        self.assertIsNotNone(library)
        sources = set(library.group(1).split())
        self.assertLessEqual({"combat_active.cpp", "combat_install.cpp", "equip_refire.cpp", "match_tags.cpp"}, sources)
        options = re.search(r"target_compile_options\(nevr_quest_combat PRIVATE([^)]*)\)", cmake)
        self.assertIsNotNone(options)
        for flag in ("-Wall", "-Wextra", "-Werror", "-fno-exceptions", "-fvisibility=hidden"):
            self.assertIn(flag, options.group(1))
        self.assertIn("target_link_libraries(nevr_quest_combat PUBLIC nevr_quest_got_hook)", cmake)

    def test_the_quest_project_adds_the_combat_directory_after_the_sentinel(self):
        cmake = read("src/quest/CMakeLists.txt")
        self.assertIn("add_subdirectory(combat)", cmake)
        self.assertLess(cmake.index("add_subdirectory(sentinel)"), cmake.index("add_subdirectory(combat)"))

    def test_the_sentinel_links_it(self):
        cmake = read("src/quest/sentinel/CMakeLists.txt")
        link = re.search(r"target_link_libraries\(ovrplatformloader PRIVATE([^)]*)\)", cmake)
        self.assertIsNotNone(link)
        self.assertIn("nevr_quest_combat", link.group(1).split())

    def test_the_budget_has_its_two_handles(self):
        budget = read("src/quest/sentinel/hook_budget.h")
        self.assertIn('{"combat/match_tags.cpp", 2, 0}', budget)

    def test_every_launch_installs_it(self):
        body = extract_braced_function(
            strip_comments(read("src/quest/sentinel/entry.cpp")), "bool InstallPermissionWaitHook() noexcept {"
        )
        self.assertEqual(body.count("nevr_quest::combat::match_tags::Install();"), 1)

    def test_the_host_test_is_a_target_of_the_combat_group(self):
        cmake = read("src/quest/host_tests/groups/combat.cmake")
        self.assertRegex(
            cmake,
            r"nevr_ht_test\(match_tag_default_test GROUP combat[^)]*src/quest/tests/match_tag_default_test\.cpp"
            r"\s+src/quest/combat/match_tags\.cpp\)",
        )


class TheMatchTagsHookOnlyDefaultsTheTwoFlagWords(unittest.TestCase):
    def source(self):
        return strip_comments(read("src/quest/combat/match_tags.cpp"))

    def test_it_installs_two_hooks_and_no_others(self):
        code = self.source()
        self.assertEqual(len(re.findall(r"InstallThunk<", code)), 2)
        self.assertIn("InstallThunk<C1Thunk>(g_c1Hook, c1)", code)
        self.assertIn("InstallThunk<C2Thunk>(g_c2Hook, c2)", code)

    def test_the_handlers_call_the_original_first(self):
        code = self.source()
        for name, tag in (("void C1Handler(", "c1"), ("void C2Handler(", "c2")):
            body = extract_braced_function(code, name)
            self.assertRegex(body, r"original\(game, args\);\s*AfterConstruction\(\"%s\", game\);" % tag)

    def test_the_handlers_do_not_log_themselves(self):
        code = self.source()
        for name in ("void C1Handler(", "void C2Handler("):
            body = extract_braced_function(code, name)
            self.assertNotIn("LogFields", body)

    def test_only_the_two_flag_offsets_are_written(self):
        code = self.source()
        stores = re.findall(r"StoreFlags\(game, (\w+), \w+\)", code)
        self.assertEqual(sorted(stores), ["kCurrentFlagsOffset", "kPendingFlagsOffset"])
        header = strip_comments(read("src/quest/combat/match_tags.h"))
        self.assertRegex(header, r"kCurrentFlagsOffset = 0x8c08;")
        self.assertRegex(header, r"kPendingFlagsOffset = 0x8ce4;")
        self.assertRegex(header, r"kStockFlags = 0x1003;")
        self.assertRegex(header, r"kMatchTagDisplayBit = 0x2000;")
        self.assertIn("flags == kStockFlags ? (flags | kMatchTagDisplayBit) : flags", header)


if __name__ == "__main__":
    unittest.main()
