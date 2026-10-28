"""#1132: the sentinel's constructor runs Attach, and everything that needs a verified libr15 follows it (docs/adr/0021).

production_steps.cpp is the one translation unit no host recipe compiles (it is the device's wiring), so the calls it must
make are pinned by source shape here; the behaviour behind them is tested on the host (libr15_attach_step_test,
integration_sequence_test, row_hooks_install_test, credits_text_test).
"""

import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
QUEST = ROOT / "src/quest"


def code(rel: str) -> str:
    text = (QUEST / rel).read_text()
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    return re.sub(r"//[^\n]*", "", text)


def body(text: str, signature: str) -> str:
    """The brace-balanced body of the first function whose declaration contains `signature`."""
    start = text.index(signature)
    open_at = text.index("{", start)
    depth = 0
    for i in range(open_at, len(text)):
        depth += {"{": 1, "}": -1}.get(text[i], 0)
        if depth == 0:
            return text[open_at : i + 1]
    raise AssertionError(f"unbalanced body for {signature}")


class AttachIsWired(unittest.TestCase):
    def test_the_sequence_runs_attach_after_the_reporter_and_before_the_early_plugins(self):
        seq = code("integration/ctor_sequence.cpp")
        order = [seq.index(f"r.Run(StepId::{step}") for step in ("kStartReporter", "kAttachLibR15", "kRunEarlyPlugins")]
        self.assertEqual(order, sorted(order))
        header = code("integration/ctor_sequence.h")
        ids = [header.index(step) for step in ("kStartReporter,", "kAttachLibR15,", "kRunEarlyPlugins,")]
        self.assertEqual(ids, sorted(ids))
        self.assertIn("virtual bool AttachLibR15() = 0;", header)
        self.assertIn('"attach_libr15"', seq)

    def test_the_production_step_runs_attach_with_the_build_options(self):
        steps = code("integration/production_steps.cpp")
        step = body(steps, "bool AttachLibR15() override")
        self.assertIn("RunAttachStep(BuildAttachOptions())", step)
        self.assertIn("return outcome.verified", step)
        self.assertIn("detail_", step)

    def test_the_identity_is_registered_by_the_step_unit_once_and_nowhere_else(self):
        unit = code("integration/libr15_attach_step.cpp")
        once = body(unit, "void RegisterIdentityOnce()")
        self.assertIn("g_identityRegistered.exchange(true", once)
        self.assertIn("plugin::SetLibr15PatchSource(&PatchStateForPlugins)", once)
        self.assertIn("sentinel::SetBuildIdAlias(&BuildIdAliasForPlugins)", once)
        # Only a verified file registers it.
        run = body(unit, "AttachStepOutcome RunAttachStep(")
        self.assertRegex(run, r"if \(outcome\.verified\) RegisterIdentityOnce\(\);")
        callers = []
        for path in sorted(QUEST.rglob("*")):
            rel = path.relative_to(QUEST).as_posix()
            if path.suffix not in (".cpp", ".h") or rel.startswith("tests/"):
                continue
            text = code(rel)
            if re.search(r"\b(SetLibr15PatchSource|SetBuildIdAlias)\s*\(", text):
                callers.append(rel)
        self.assertEqual(
            callers,
            [
                "integration/libr15_attach_step.cpp",
                "plugin/plugin_platform.cpp",
                "plugin/plugin_platform.h",
                "sentinel/got_hook.cpp",
                "sentinel/got_hook.h",
            ],
        )

    def test_the_attach_step_unit_is_built_into_the_integration_library(self):
        self.assertIn("libr15_attach_step.cpp", (QUEST / "integration/CMakeLists.txt").read_text())

    def test_credits_and_the_three_row_features_are_installed_by_the_steps(self):
        steps = code("integration/production_steps.cpp")
        credits = body(steps, "bool InstallCreditsText() override")
        self.assertIn("nevr_quest::credits_text::InstallCreditsText()", credits)
        social = body(steps, "bool InstallSocial() override")
        self.assertIn("quest_social::lobby_full::InstallLobbyFullHooks()", social)
        self.assertIn("nevr_quest::ui_texture_mips::InstallUiTextureMips()", social)
        self.assertIn("nevr_quest::splash_skip::Install()", social)

    def test_no_step_logs_a_skipped_code_write_any_more(self):
        for path in sorted(QUEST.rglob("*")):
            if path.suffix in (".cpp", ".h"):
                text = path.read_text()
                self.assertNotIn("LogCodeWriteSkipped", text, path.relative_to(ROOT).as_posix())
                self.assertNotIn("code_write_skipped", text, path.relative_to(ROOT).as_posix())

    def test_the_hook_free_translation_unit_keeps_its_exceptions_and_the_thunk_header_out(self):
        steps = (QUEST / "integration/production_steps.cpp").read_text()
        self.assertNotRegex(steps, r'#include\s+"[^"]*callback_thunk\.h"')
        for header in ("sentinel/credits_text.h", "sentinel/splash_skip_hook.h", "sentinel/ui_texture_mips.h", "social/lobby_full_hooks.h",
                       "integration/libr15_attach_step.h"):
            self.assertNotRegex((QUEST / header).read_text(), r'#include\s+"[^"]*callback_thunk\.h"', header)


class ExtraRowsAreAVariantBuildSwitch(unittest.TestCase):
    """docs/adr/0021 section 11: the public build carries no extra row, no parser and no digest."""

    def test_the_cache_variable_is_empty_by_default(self):
        cmake = (QUEST / "CMakeLists.txt").read_text()
        self.assertRegex(cmake, r'set\(NEVR_LIBR15_EXTRA_ROWS "" CACHE FILEPATH')

    def test_the_parser_is_linked_only_when_the_variable_is_set(self):
        text = (QUEST / "sentinel/CMakeLists.txt").read_text()
        block = text[text.index("if(NEVR_LIBR15_EXTRA_ROWS)") : text.index("endif()", text.index("if(NEVR_LIBR15_EXTRA_ROWS)"))]
        self.assertIn("libr15_extra_rows.cpp", block)
        self.assertIn("NEVR_LIBR15_EXTRA_ROWS=1", block)
        self.assertIn("nevr_libr15_extra_rows(", block)
        outside = text.replace(block, "")
        self.assertNotIn("libr15_extra_rows", outside)

    def test_no_public_recipe_and_no_preset_passes_a_file(self):
        # Only the host test recipe names it (it configures a harness project with the example fixture).
        text = (ROOT / "justfile").read_text()
        recipes = re.split(r"(?m)^(?=[A-Za-z_][\w-]*[^\n]*:\s*(?:\(.*\))?\s*$)", text)
        users = [r.split("\n", 1)[0] for r in recipes if "NEVR_LIBR15_EXTRA_ROWS" in r]
        self.assertEqual(len(users), 1, users)
        self.assertTrue(users[0].startswith("test-quest-hooks"), users)
        self.assertNotIn("NEVR_LIBR15_EXTRA_ROWS", (QUEST / "CMakePresets.json").read_text())

    def test_the_identity_switch_is_a_compile_definition_and_the_default_is_empty(self):
        identity = code("sentinel/libr15_identity.cpp")
        self.assertIn("#ifdef NEVR_LIBR15_EXTRA_ROWS", (QUEST / "sentinel/libr15_identity.cpp").read_text())
        self.assertIn("__builtin_trap()", identity)  # an unparsable embedded text never falls back to the public rows
        self.assertIn('return "";', identity)

    def test_the_attach_options_carry_the_rows_and_the_digest(self):
        unit = code("integration/libr15_attach_step.cpp")
        self.assertIn("options.extraRows = sentinel::pinned::LibR15ExtraRows();", unit)
        self.assertIn("options.extraRowsSha256 = sentinel::pinned::LibR15ExtraRowsSha256();", unit)


if __name__ == "__main__":
    unittest.main()
