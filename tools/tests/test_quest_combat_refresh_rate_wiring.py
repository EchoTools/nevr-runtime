"""#2172, #2000: the refresh-rate rule and the game tick are the runtime's, and the q15t plugin installs on neither slot.

refresh_rate_test.cpp, combat_tick_test.cpp and render_config_hook_test.cpp drive the behaviour (the rule, the tick chain on a fixture
slot in both orders against the plugin's own code, the handler's original getting the rule's answer). This file pins what they cannot see:
the handler asks the rule before it calls the original and reports after, the registration table's order, the tick's install goes through
the manager and the plugin's source no longer names the two vrapi slots or the options of the deleted modules, the keys exist in every
place that lists them, and the config marker the build checks no longer carries the deleted option.
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


class TheRenderConfigHandlerAsksTheOwner(unittest.TestCase):
    def body(self):
        code = strip_comments(read("src/quest/sentinel/render_config_hook.cpp"))
        return extract_braced_function(code, "int OnSetDisplayRefreshRate(")

    def test_choose_comes_before_the_original_and_note_after_it(self):
        body = self.body()
        choose = body.index("combat::refresh_rate::Choose(ovr, rate)")
        original = body.index("original(ovr, sent)")
        note = body.index("combat::refresh_rate::Note(rate, sent, result)")
        self.assertLess(choose, original)
        self.assertLess(original, note)

    def test_the_original_is_never_called_with_the_games_own_rate_directly(self):
        self.assertNotIn("original(ovr, rate)", self.body())

    def test_the_rate_the_line_reports_is_the_one_the_original_was_called_with(self):
        self.assertIn("ToMilli(sent)", self.body())
        self.assertNotIn("ToMilli(rate)", self.body())


class TheTableAndTheTick(unittest.TestCase):
    def test_the_tick_installs_before_the_rule_in_the_normal_phase(self):
        table = strip_comments(read("src/quest/combat/combat_install.cpp"))
        tick = table.index('Module{"tick", nullptr, &tick::InstallNormal}')
        rule = table.index('Module{"refresh_rate", nullptr, &refresh_rate::InstallNormal}')
        self.assertLess(tick, rule)

    def test_the_tick_is_a_thunk_installed_through_the_manager_with_no_expected_original(self):
        code = strip_comments(read("src/quest/combat/combat_tick.cpp"))
        self.assertIn("sentinel::InstallThunk<Thunk>(g_hook, targets::Target(), lookup)", code)
        self.assertIn("NEVR_HOOK_RECORD(kTickHook, Thunk, &OnTick);", code)
        targets = strip_comments(read("src/quest/combat/combat_tick_targets.h"))
        # the plugin's wrapper may be in the slot first: the first-handler rule takes it as the original, so no pin of the stock function
        self.assertNotRegex(targets, r"GotTarget\{[^}]*kStockVaddr")
        self.assertIn(".Owned(\"combat_tick\")", targets)

    def test_the_handler_calls_the_original_before_the_callbacks(self):
        code = strip_comments(read("src/quest/combat/combat_tick.cpp"))
        body = extract_braced_function(code, "void OnTick(")
        self.assertLess(body.index("original(game);"), body.index("fn(object)"))

    def test_the_tick_has_a_hook_budget_row(self):
        self.assertIn('{"combat/combat_tick.cpp", 1, 0}', read("src/quest/sentinel/hook_budget.h"))

    def test_the_plugins_tick_is_marked_known_wrong_where_the_runtime_joins_it(self):
        for path in ("src/quest/combat/combat_tick.h", "src/quest/combat/combat_tick.cpp"):
            self.assertIn("KNOWN-WRONG #2168", read(path))

    def test_the_module_reads_the_headset_itself_only_behind_a_known_wrong_marker(self):
        code = read("src/quest/combat/refresh_rate.cpp")
        for match in re.finditer(r'ro\.product\.device', code):
            self.assertIn("KNOWN-WRONG #2037", code[max(0, match.start() - 900):match.start()])

    def test_the_resolved_section_reaches_the_modules_before_the_line_is_written(self):
        code = strip_comments(read("src/quest/sentinel/entry.cpp"))
        body = extract_braced_function(code, "void LogCombatConfig(")
        self.assertLess(body.index("combat::SetConfig(config)"), body.index("combat::LogConfig(config)"))


class TheKeysAreListedEverywhere(unittest.TestCase):
    KEYS = ("refresh_rate", "refresh_rate_lobby", "refresh_rate_quest2_72")

    def test_each_key_is_read_known_logged_and_documented(self):
        reader = read("src/quest/sentinel/quest_config.cpp")
        schema = read("src/core/nevr_config.cpp")
        line = read("src/quest/combat/combat_install.cpp")
        example = read("docs/reference/example-config.yaml")
        for key in self.KEYS:
            self.assertIn(f'"combat.{key}"', reader)
            self.assertIn(f'"{key}"', schema)
            self.assertIn(f'"{key}"', line)
            self.assertRegex(example, rf"\n  # {key}: ")

    def test_each_key_defaults_to_on(self):
        header = read("src/quest/sentinel/quest_config.h")
        self.assertRegex(header, r"struct CombatSwitchChoice \{\n  bool on = true;")
        self.assertIn("ReadBool(cfg, key, true, r)", read("src/quest/sentinel/quest_config.cpp"))


class ThePluginInstallsOnNeitherSlotAndTheDeletedModulesAreGone(unittest.TestCase):
    def test_the_deleted_files_are_gone(self):
        for name in ("refresh_rate", "clock_levels", "plugin_args"):
            for ext in ("h", "cpp"):
                self.assertFalse((ROOT / f"src/quest/plugins/q15t/src/{name}.{ext}").exists(), f"{name}.{ext}")

    def test_the_plugin_source_does_not_name_the_two_vrapi_slots(self):
        for path in (ROOT / "src/quest/plugins/q15t/src").iterdir():
            if path.suffix not in (".cpp", ".h", ".inc"):
                continue
            code = strip_comments(path.read_text())
            self.assertNotIn("vrapi_SetClockLevels", code, path.name)
            self.assertNotIn("vrapi_SetDisplayRefreshRate", code, path.name)
            self.assertNotIn("Q15T_HZ_MATCH90", code, path.name)
            self.assertNotIn("SetClockRaise", code, path.name)

    def test_the_build_has_no_option_for_the_deleted_modules(self):
        cmake = read("src/quest/plugins/q15t/CMakeLists.txt")
        for token in ("Q15T_HZ_MATCH90", "Q15T_CPU_LEVEL", "Q15T_GPU_LEVEL", "clock_levels", "refresh_rate", "plugin_args"):
            self.assertNotIn(token, cmake)
        self.assertIn("src/combat_map.cpp", cmake)  # the detector aibots and music read stays until they move

    def test_the_config_marker_and_the_recipe_check_agree_without_the_option(self):
        marker = read("src/quest/plugins/q15t/src/plugin.cpp")
        self.assertNotIn("hz_level", marker)
        self.assertNotIn("hz_level", read("justfile"))

    def test_the_init_line_no_longer_reports_a_clock_candidate_or_a_hz_mode(self):
        code = strip_comments(read("src/quest/plugins/q15t/src/plugin.cpp"))
        for token in ("clock_candidate", "clock_arg", "hz_mode", "ArgString"):
            self.assertNotIn(token, code)

    def test_the_two_decisions_moved_with_their_adrs(self):
        self.assertTrue((ROOT / "docs/quest/combat/adr/0001-match-refresh-rate.md").is_file())
        self.assertTrue((ROOT / "docs/quest/combat/adr/0002-level-refresh-rate-default.md").is_file())
        self.assertFalse((ROOT / "docs/quest/q15t/adr/0001-match-refresh-rate.md").exists())
        self.assertFalse((ROOT / "docs/quest/q15t/adr/0002-level-refresh-rate-default.md").exists())


if __name__ == "__main__":
    unittest.main()
