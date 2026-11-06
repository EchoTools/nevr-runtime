"""#2171: the combat area's two install steps are wired into the constructor sequence and the post-load policy, and gated by combat::Active().

combat_install_test.cpp drives the steps over an injected table, and integration_sequence_test.cpp drives the sequence and the post-load
policy over fakes. This file pins what neither can see: that the production steps ask the real gate, that the post-load action and the
process end reach the real steps, that the `combat:` section is reported once, and that the group, the recipe and the verify entry exist.
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
        self.assertIn("nevr_quest::integration::InstallCombatNormal();", action)
        install = extract_braced_function(
            self.steps, "bool InstallDlopenHook(bool login, bool matchmaking, bool plugins, bool combat) override {"
        )
        self.assertIn("if (combat) actions.combat = &CombatAction;", install)
        normal = extract_braced_function(self.entry, "void InstallCombatNormal() noexcept {")
        self.assertIn("nevr_quest::combat::InstallNormal();", normal)

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
        self.assertIn("void LogCombatConfig(const nevr_quest::CombatConfig& config) noexcept { nevr_quest::combat::LogConfig(config); }", self.entry)

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

    def test_verify_runs_the_recipe_beside_the_q15t_one(self):
        justfile = read("justfile")
        self.assertRegex(justfile, r"test-quest-q15t \\\n\s+test-quest-combat \\\n")
        self.assertRegex(justfile, r"\ntest-quest-combat module=\"\":")

    def test_the_config_section_has_its_key_and_the_example_config_shows_it(self):
        self.assertIn('{"combat", {"force"}}', read("src/core/nevr_config.cpp"))
        self.assertRegex(read("docs/reference/example-config.yaml"), r"\ncombat:\n")


if __name__ == "__main__":
    unittest.main()
