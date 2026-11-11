"""#1601: the menu gate's default library list names the lobby script, so the lobby loads its combat wing on a Quest.

The lobby script (libf808c072bb49da47.so) holds two LoadLevel branch nodes (4470b75a, a1ebf03c) that read the headset-type
expression's quest output: a Quest gets the arena wing, any other kind gets the arena and combat wings. The gate answers the
expression's Bind with the Link kind (4) for the libraries in kDefaultLinkLibs (src/quest/combat/headset_gates.h), so the quest output
stays false in that library. The list is a constant that no host test compiles against the real game, so it is pinned by source shape here.
"""

import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
COMBAT = ROOT / "src/quest/combat"
LOBBY_SCRIPT = "libf808c072bb49da47.so"


def default_libs() -> list[str]:
    text = (COMBAT / "headset_gates.h").read_text()
    m = re.search(r"kDefaultLinkLibs\[\] =((?:\s*\"[^\"]*\")+);", text)
    assert m, "headset_gates.h has no kDefaultLinkLibs"
    return "".join(re.findall(r'"([^"]*)"', m.group(1))).split(",")


class LinkGateLibs(unittest.TestCase):
    def test_the_lobby_script_is_answered(self):
        self.assertIn(LOBBY_SCRIPT, default_libs())

    def test_the_twenty_combat_menu_scripts_are_still_answered(self):
        menu = [lib for lib in default_libs() if lib != LOBBY_SCRIPT]
        self.assertEqual(len(menu), 20)
        self.assertEqual(len(set(menu)), 20)

    def test_every_entry_is_a_script_library_file_name(self):
        for lib in default_libs():
            self.assertRegex(lib, r"^lib[0-9a-f]{16}\.so$")

    def test_the_list_fits_the_config_text_buffer(self):
        header = (COMBAT / "headset_gates.h").read_text()
        config = (ROOT / "src/quest/sentinel/quest_config.h").read_text()
        size = int(re.search(r"kMaxLinkLibsText = (\d+);", header).group(1))
        self.assertEqual(size, int(re.search(r"kCombatTextMax = (\d+);", config).group(1)))
        self.assertLess(len(",".join(default_libs())) + 1, size)

    def test_the_gate_answers_a_real_non_quest_kind(self):
        header = (COMBAT / "headset_gates.h").read_text()
        self.assertRegex(header, r"kLinkHeadset = 4;")

    def test_no_plugin_carries_a_second_list(self):
        # The q15t plugin, which carried its own list, is deleted (#2189): the runtime's list is the only one.
        self.assertFalse((ROOT / "src/quest/plugins/q15t").exists())


if __name__ == "__main__":
    unittest.main()
