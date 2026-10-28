"""#747: the Quest main menu shows the runtime version through a GOT slot, never a code page.

The versions panel sets its lobby_version text through CCanvasUICS::SetText, which libr15 calls only through its own
PLT stub (JUMP_SLOT 0x36ff908, a data page; src/quest/sentinel/pinned_targets.h). The host test
(src/quest/tests/menu_version_label_test.cpp) proves what the handler does; the pinned-library test
(src/quest/tests/got_pinned_test.cpp) proves the slot in the real libr15. This pins the source shape that joins them:
the install is a GOT install of that one target, nothing in the feature writes a code page or changes a protection, the
social step calls it, and the recipe runs the host test.
"""

import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
FEATURE = ROOT / "src/quest/social/menu_version_label.cpp"
HEADER = ROOT / "src/quest/social/menu_version_label.h"
HOOK_HEADER = ROOT / "src/quest/social/menu_version_label_hook.h"
STEPS = ROOT / "src/quest/integration/production_steps.cpp"
TARGETS = ROOT / "src/quest/sentinel/pinned_targets.h"
JUSTFILE = ROOT / "justfile"


def strip_comments(text: str) -> str:
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    return re.sub(r"//[^\n]*", "", text)


class QuestMenuVersionIsAGotHook(unittest.TestCase):
    def test_the_feature_installs_one_got_thunk_on_the_pinned_target(self):
        text = strip_comments(FEATURE.read_text())
        self.assertGreater(len(text), 500)  # a sensor that reads nothing watches nothing
        self.assertIn("sentinel::InstallThunk<Thunk>(g_hook, target)", text)
        self.assertIn("InstallAt(sentinel::pinned::LibR15CanvasSetText(), fitted.c_str())", text)
        self.assertEqual(len(re.findall(r"InstallThunk\s*<", text)), 1)

    def test_the_feature_writes_no_code_and_changes_no_protection(self):
        for path in (FEATURE, HEADER, HOOK_HEADER):
            text = strip_comments(path.read_text())
            self.assertNotRegex(text, r"InstallInlineThunk|InlineHook|\bWriteWord\b|\bmprotect\s*\(|PROT_EXEC", path.name)
            self.assertNotIn("inline_hook.h", text, path.name)

    def test_the_public_header_keeps_the_thunk_out_of_exception_code(self):
        # production_steps.cpp is built with exceptions; callback_thunk.h refuses to be included there.
        for path in (HEADER, STEPS):
            text = strip_comments(path.read_text())
            self.assertNotIn("menu_version_label_hook.h", text, path.name)
            self.assertNotIn("pinned_targets.h", text, path.name)
            self.assertNotIn("callback_thunk.h", text, path.name)
        self.assertIn("callback_thunk.h", strip_comments(HOOK_HEADER.read_text()) + strip_comments(TARGETS.read_text()))

    def test_the_target_is_the_one_measured_slot(self):
        text = TARGETS.read_text()
        self.assertIn(
            '"_ZN10NRadEngine11CCanvasUICS7SetTextENS_18SCompactPoolHandleENS_9CSymbol64EPKc"', text
        )
        m = re.search(
            r"inline GotTarget LibR15CanvasSetText\(\) \{\s*return \{kLibR15, kCanvasSetTextSymbol, RelocKind::kJumpSlot, "
            r"kLibR15BuildId, (0x[0-9a-f]+)ULL\};",
            text,
        )
        self.assertIsNotNone(m, "the target is not a pinned JUMP_SLOT on the pinned build ID")
        self.assertEqual(m.group(1), "0x36ff908")

    def test_the_social_step_installs_it_and_no_longer_skips_it(self):
        text = strip_comments(STEPS.read_text())
        self.assertIn("if (ok) quest_social::menu_version::InstallMenuVersionLabelHook();", text)
        self.assertIn('#include "quest/social/menu_version_label.h"', STEPS.read_text())
        self.assertNotIn('LogCodeWriteSkipped("menu_version")', text)
        # It rides the social step after the facade is in place, the way the lobby-full policy did.
        body = text[text.index("bool InstallSocial()") :]
        self.assertLess(body.index("InstallSocialHook"), body.index("InstallMenuVersionLabelHook"))

    def test_the_recipe_runs_the_host_test_with_the_real_handler(self):
        text = JUSTFILE.read_text()
        self.assertRegex(
            text,
            r"src/quest/tests/menu_version_label_test\.cpp\s*\\\n\s*src/quest/social/menu_version_label\.cpp",
        )
        self.assertIn('timeout 120 "$out/menu_version_label_test"', text)


if __name__ == "__main__":
    unittest.main()
