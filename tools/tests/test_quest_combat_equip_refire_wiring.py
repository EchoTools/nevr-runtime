"""#2121: the equipment re-fire on Quest is a combat module over the shared rule header, built into the sentinel.

equip_refire_test.cpp drives the real wrappers through the six installed words and got_pinned_test.cpp checks the six relocations
and the code pins in the real libr15. This file pins what neither can see: that the rule is the shared header's and is not copied
into the Quest file, the combat target, the sentinel's install and budget row, the host test's registration, that the module
changes no code page, and that nothing of it is left in the q15t plugin.

It also holds the wiring of the combat area's two install steps (#2171): combat_install_test.cpp drives the steps over an injected
table and integration_sequence_test.cpp drives the sequence and the post-load policy over fakes; the classes at the end pin what neither
can see (the production steps ask the real gate, the post-load action and the process end reach the real steps, the `combat:` section is
reported once, the group, the recipe and the verify entry exist).
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
        block = re.search(r"add_library\(nevr_quest_combat STATIC([^)]*)\)", cmake)
        self.assertIsNotNone(block)
        # The list grows with each module that moves in, so each file is checked, not the whole list.
        for name in ("combat_active.cpp", "combat_install.cpp", "equip_refire.cpp", "match_tags.cpp"):
            self.assertIn(name, block.group(1).split())

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

    def test_the_host_test_is_a_target_of_the_combat_group(self):
        cmake = read("src/quest/host_tests/groups/combat.cmake")
        self.assertRegex(
            cmake,
            r"nevr_ht_test\(equip_refire_test GROUP combat[^)]*src/quest/tests/equip_refire_test\.cpp"
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
    def test_the_plugin_and_its_tests_are_gone(self):
        # The q15t plugin and its host tests are deleted (#2189): no second copy of the re-fire can come back through them.
        for path in ("src/quest/plugins/q15t", "src/quest/tests/q15t", "docs/quest/q15t"):
            self.assertFalse((ROOT / path).exists(), path)


class TheProductionStepsReachTheRealCombatSteps(unittest.TestCase):
    def setUp(self):
        self.steps = strip_comments(read("src/quest/integration/production_steps.cpp"))
        self.entry = strip_comments(read("src/quest/sentinel/entry.cpp"))

    def test_the_sequence_asks_the_real_gate(self):
        self.assertIn("bool CombatActive() override { return nevr_quest::integration::CombatActive(); }", self.steps)
        self.assertIn("bool CombatActive() noexcept { return nevr_quest::combat::Active(); }", self.entry)

    def test_the_early_step_is_the_areas_install_early(self):
        self.assertIn("bool InstallCombatEarly() override { return nevr_quest::integration::InstallCombatEarly(); }", self.steps)
        self.assertIn("bool InstallCombatEarly() noexcept { return nevr_quest::combat::InstallEarly().ran; }", self.entry)

    def test_the_post_load_action_runs_the_normal_step_and_the_dlopen_install_sets_it_only_when_asked(self):
        action = extract_braced_function(self.steps, "ActionResult CombatAction() noexcept {")
        self.assertIn('InstallCombatNormal() ? "ran" : "ended"', action)
        install = extract_braced_function(
            self.steps, "bool InstallDlopenHook(bool login, bool matchmaking, bool plugins, bool combat) override {"
        )
        self.assertIn("if (combat) actions.combat = &CombatAction;", install)
        normal = extract_braced_function(self.entry, "bool InstallCombatNormal() noexcept {")
        self.assertIn("return nevr_quest::combat::InstallNormal().ran;", normal)

    def test_a_normal_step_that_never_ran_is_recorded_when_the_hook_is_not_live_and_when_the_process_ends(self):
        self.assertIn("void FinishCombat(const char* reason) override { nevr_quest::integration::NoteCombatNotStarted(reason); }", self.steps)
        end = extract_braced_function(self.steps, "void OnGameProcessEnd() noexcept {")
        self.assertIn("nevr_quest::integration::NoteCombatNotStarted(nevr_quest::plugin::reason::kGameExitedFirst);", end)
        self.assertLess(end.index("nevr_quest::plugin::FinishPlugins("), end.index("NoteCombatNotStarted("))
        self.assertIn(
            "void NoteCombatNotStarted(const char* reason) noexcept { nevr_quest::combat::NoteNormalNotStarted(reason); }", self.entry
        )

    def test_the_combat_section_is_reported_once_before_the_gate_makes_its_answer(self):
        body = extract_braced_function(self.steps, "const nevr_quest::ResolvedConfig& ResolveConfig() override {")
        self.assertEqual(body.count("LogCombatConfig("), 1)
        self.assertLess(body.index("LogCombatConfig(cfg.combat)"), body.index("ResolveCombatActive("))
        self.assertIn("void LogCombatConfig(const nevr_quest::CombatConfig& config) noexcept {", self.entry)
        self.assertIn("nevr_quest::combat::LogConfig(config);", self.entry)

    def test_the_configuration_logs_no_second_line_for_the_same_key(self):
        config = strip_comments(read("src/quest/sentinel/quest_config.cpp"))
        report = extract_braced_function(config, "void ReportState(LoadResult& r) {")
        self.assertNotIn("kCombatForceKey", report)


class TheSequenceNamesTheStepAndOrdersIt(unittest.TestCase):
    def test_the_early_step_sits_between_the_early_plugin_pass_and_the_clock_hook(self):
        header = strip_comments(read("src/quest/integration/ctor_sequence.h"))
        enum = header[header.index("enum class StepId"):]
        self.assertRegex(enum, r"kAttachLibR15,\s*kRunEarlyPlugins,\s*kInstallCombatEarly,\s*kInstallClockHook,")
        sequence = strip_comments(read("src/quest/integration/ctor_sequence.cpp"))
        self.assertLess(sequence.index("StepId::kRunEarlyPlugins, [&]"), sequence.index("StepId::kInstallCombatEarly, [&]"))
        self.assertLess(sequence.index("StepId::kInstallCombatEarly, [&]"), sequence.index("StepId::kInstallClockHook, [&]"))

    def test_the_sequence_asks_the_gate_and_passes_it_to_the_dlopen_install(self):
        sequence = strip_comments(read("src/quest/integration/ctor_sequence.cpp"))
        self.assertIn("combatNormal = steps.CombatActive();", sequence)
        self.assertIn("pluginsNormal || combatNormal;", sequence)
        self.assertIn("steps.InstallDlopenHook(loginWanted, matchmakingWanted, pluginsNormal, combatNormal)", sequence)
        self.assertIn("steps.FinishCombat(", sequence)


class TheGroupTheRecipeAndTheVerifyEntryExist(unittest.TestCase):
    def test_the_install_step_is_part_of_the_combat_target(self):
        self.assertRegex(read("src/quest/combat/CMakeLists.txt"), r"combat_active\.cpp\s+combat_install\.cpp\s")

    def test_the_group_builds_the_install_test(self):
        cmake = read("src/quest/host_tests/groups/combat.cmake")
        self.assertIn("nevr_ht_group(combat)", cmake)
        self.assertRegex(
            cmake,
            r"nevr_ht_test\(combat_install_test GROUP combat[^)]*src/quest/tests/combat_install_test\.cpp"
            r"\s+src/quest/combat/combat_install\.cpp",
        )

    def test_verify_runs_the_combat_recipe_and_names_no_q15t_one(self):
        justfile = read("justfile")
        verify = justfile.split("\nverify:", 1)[1]
        self.assertRegex(verify, r"\n\s+test-quest-combat \\\n")
        self.assertNotIn("test-quest-q15t", justfile)
        self.assertRegex(justfile, r"\ntest-quest-combat module=\"\":")

    def test_the_config_section_has_its_key_and_the_example_config_shows_it(self):
        # The section grows with each module that moves in, so each key the install steps read is checked, not the whole list.
        section = re.search(r'\{"combat", \{([^}]*)\}\}', read("src/core/nevr_config.cpp"))
        self.assertIsNotNone(section, "nevr_config.cpp has no combat section")
        keys = re.findall(r'"([a-z0-9_]+)"', section.group(1))
        for key in ("force", "heaps", "memsys", "refresh_rate", "refresh_rate_lobby", "refresh_rate_quest2_72"):
            with self.subTest(key=key):
                self.assertIn(key, keys)
        self.assertRegex(read("docs/reference/example-config.yaml"), r"\ncombat:\n")


if __name__ == "__main__":
    unittest.main()
