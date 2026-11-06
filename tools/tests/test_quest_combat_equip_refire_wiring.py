"""#2121: the equipment re-fire on Quest is a combat module over the shared rule header, built into the sentinel.

equip_refire_test.cpp drives the real wrappers through the six installed words and got_pinned_test.cpp checks the six relocations
and the code pins in the real libr15. This file pins what neither can see: that the rule is the shared header's and is not copied
into the Quest file, the combat target, the sentinel's install and budget row, the host test's registration, that the module
changes no code page, and that nothing of it is left in the q15t plugin.
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


class TheRuleIsTheSharedHeaders(unittest.TestCase):
    def source(self):
        return strip_comments(read("src/quest/combat/equip_refire.cpp"))

    def test_the_quest_file_includes_the_shared_rule_header(self):
        self.assertIn('#include "runtime/patch/equipment_refire_rules.h"', read("src/quest/combat/equip_refire.cpp"))

    def test_the_setter_wrapper_hands_the_call_to_the_rule(self):
        code = self.source()
        body = extract_braced_function(code, "void SetterHandler(")
        self.assertRegex(body, r"rules::OnSetter\(env, g_tracker,")
        self.assertRegex(body, r"original\(component, handle, value\)")

    def test_the_frame_work_calls_the_rules_fire_due(self):
        code = self.source()
        self.assertRegex(extract_braced_function(code, "void FireDueNow() noexcept {"), r"rules::FireDue\(env, g_tracker\)")

    def test_no_rule_is_copied_into_the_quest_file(self):
        # The debounce, the staleness reasons, the layout offsets and the settle time are the header's; a copy here could drift from the PC build.
        code = self.source()
        for pattern in (
            r"\bclass\s+Debouncer\b",
            r"\bstruct\s+Tracker\s*\{",
            r"\bkSettleMs\s*=",
            r"record_changed_elsewhere",
            r"component_replaced",
            r"\b0xc8\b",
            r"\b0xf8\b",
            r"\b0x58\b",
        ):
            self.assertIsNone(re.search(pattern, code), pattern)

    def test_the_only_game_offset_in_the_glue_is_the_net_game_field(self):
        code = self.source()
        self.assertRegex(code, r"kNetGameField = 0x8510;")

    def test_the_wrappers_call_the_original_before_anything_else_of_theirs(self):
        # The rule reads the record before the original runs and judges after it; the wrapper adds nothing around the call.
        body = extract_braced_function(self.source(), "void SetterHandler(")
        self.assertNotIn("LogFields", body)
        self.assertNotIn("Lock", body)


class TheModuleIsWired(unittest.TestCase):
    def test_the_combat_target_builds_it(self):
        cmake = read("src/quest/combat/CMakeLists.txt")
        self.assertRegex(cmake, r"add_library\(nevr_quest_combat STATIC\s+equip_refire\.cpp\s+match_tags\.cpp\)")

    def test_the_budget_has_its_six_handles(self):
        self.assertIn('{"combat/equip_refire.cpp", 6, 0}', read("src/quest/sentinel/hook_budget.h"))
        code = strip_comments(read("src/quest/combat/equip_refire.cpp"))
        self.assertRegex(code, r"constexpr std::size_t kHookCount = 6;\s*sentinel::GotHook g_hooks\[kHookCount\];")
        self.assertEqual(len(re.findall(r"InstallThunk<", code)), 1)  # one call site, run once per word

    def test_every_launch_installs_it_after_the_name_tags(self):
        body = extract_braced_function(
            strip_comments(read("src/quest/sentinel/entry.cpp")), "bool InstallPermissionWaitHook() noexcept {"
        )
        self.assertEqual(body.count("nevr_quest::combat::equip_refire::Install();"), 1)
        self.assertLess(
            body.index("nevr_quest::combat::match_tags::Install();"), body.index("nevr_quest::combat::equip_refire::Install();")
        )

    def test_the_host_test_is_a_target_of_the_hooks_group(self):
        cmake = read("src/quest/host_tests/groups/hooks.cmake")
        self.assertRegex(
            cmake,
            r"nevr_ht_test\(equip_refire_test GROUP hooks[^)]*src/quest/tests/equip_refire_test\.cpp"
            r"\s+src/quest/combat/equip_refire\.cpp\s+src/quest/invite/frame_dispatch\.cpp\)",
        )

    def test_the_pinned_test_resolves_the_words_and_the_code(self):
        text = read("src/quest/tests/got_pinned_test.cpp")
        self.assertIn("pinned::LibR15EquipmentSetter(slot, kind)", text)
        self.assertIn("pinned::kEquipmentRefireCode[i]", text)

    def test_the_six_words_are_the_three_setters_two_relocations(self):
        text = read("src/quest/sentinel/pinned_targets.h")
        for word in ("0x36f3d00ULL", "0x37233e8ULL", "0x36fa900ULL", "0x3727b38ULL", "0x36d8e10ULL", "0x3711db8ULL"):
            self.assertIn(word, text)
        for setter in ("0x217ca84ULL", "0x217cde4ULL", "0x217cf90ULL"):
            self.assertIn(setter, text)
        rows = text.split("kEquipmentRefireCode[] = {")[1].split("};")[0]
        self.assertEqual(len(re.findall(r"ULL,\s*\{0x", rows)), 16)


class TheModuleChangesNoCodePage(unittest.TestCase):
    def test_the_module_makes_no_protection_or_mapping_call(self):
        # GotHook writes the data words; the module itself only reads (N151).
        for path in ("src/quest/combat/equip_refire.cpp", "src/quest/combat/equip_refire.h"):
            code = strip_comments(read(path))
            for call in ("mprotect", "mmap", "mremap", "PROT_EXEC", "PROT_WRITE"):
                self.assertNotIn(call, code, f"{path}: {call}")

    def test_it_is_built_without_exceptions_and_no_exception_is_named(self):
        code = strip_comments(read("src/quest/combat/equip_refire.cpp"))
        for token in ("try {", "catch (", "throw "):
            self.assertNotIn(token, code)


class NothingOfItIsLeftInTheQ15tPlugin(unittest.TestCase):
    def test_the_plugin_has_no_equiprefire_file_or_mention(self):
        for path in (
            "src/quest/plugins/q15t/src/equiprefire.cpp",
            "src/quest/plugins/q15t/src/equiprefire.h",
            "src/quest/tests/q15t/equiprefire_test.cpp",
            "src/quest/tests/q15t/mod/equiprefire.cmake",
            "src/quest/tests/q15t/mutants.d/equiprefire.sh",
            "docs/quest/q15t/modules/equiprefire.md",
        ):
            self.assertFalse((ROOT / path).exists(), path)
        for path in (
            "src/quest/plugins/q15t/CMakeLists.txt",
            "src/quest/plugins/q15t/README.md",
            "src/quest/plugins/q15t/src/plugin.cpp",
            "src/quest/tests/q15t/CMakeLists.txt",
            "src/quest/tests/q15t/wiring_test.cpp",
        ):
            self.assertNotIn("equiprefire", read(path).lower(), path)


if __name__ == "__main__":
    unittest.main()
