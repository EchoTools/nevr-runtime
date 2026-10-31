"""#1601: the q15t menu gate's library list names the lobby script, so the lobby loads its combat wing on a Quest.

The lobby script (libf808c072bb49da47.so) holds two LoadLevel branch nodes (4470b75a, a1ebf03c) that read the headset-type
expression's quest output: a Quest gets the arena wing, any other kind gets the arena and combat wings. The gate answers the
expression's Bind with the Link kind (4) for the libraries in Q15T_LINK_GATE_LIBS (headset_gates.h), so the quest output stays
false in that library. The list is a CMake default that no host test compiles, so it is pinned by source shape here.
"""

import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
PLUGIN = ROOT / "src/quest/plugins/q15t"
LOBBY_SCRIPT = "libf808c072bb49da47.so"
SPEC_BUFFER = 640  # char spec[640] in plugin.cpp: the list, a NUL included, must fit


def default_libs() -> list[str]:
    text = (PLUGIN / "CMakeLists.txt").read_text()
    m = re.search(r'^set\(Q15T_LINK_GATE_LIBS "([^"]*)" CACHE STRING', text, flags=re.M)
    assert m, "CMakeLists.txt has no set(Q15T_LINK_GATE_LIBS ...)"
    return m.group(1).split(",")


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

    def test_the_list_fits_the_plugin_spec_buffer(self):
        self.assertLess(len(",".join(default_libs())) + 1, SPEC_BUFFER)
        self.assertIn(f"char spec[{SPEC_BUFFER}]", (PLUGIN / "src/plugin.cpp").read_text())

    def test_the_gate_answers_a_real_non_quest_kind(self):
        header = (PLUGIN / "src/headset_gates.h").read_text()
        self.assertRegex(header, r"kLinkHeadset = 4;")


if __name__ == "__main__":
    unittest.main()
