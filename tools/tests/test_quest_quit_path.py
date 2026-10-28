"""#1276: the main-menu QUIT reaches the headset with the system-UI type the game asks for.

libr15's `CVR::ShowSystemUI` (0x1a73990 in the pinned build) is `mov w1, #1` then a tail call through the
`vrapi_ShowSystemUI` JUMP_SLOT (0x36d5280). On Horizon OS v2.7, type 1 brings up the confirm-quit prompt and the game
exits; a hook that sent type 0 there opened the system Settings panel instead. The community-patched APK leaves this
call alone too. What each type does is the system VrApi's, which this tree cannot read (the APK's libvrapi.so forwards
the call through a table), so no Quest code hooks the slot. A change to what QUIT does starts from a headset
measurement of the type on each OS (#392), not from the enum's names. This is pinned by source shape: no host test can
run the system's VrApi.
"""

import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
QUEST = ROOT / "src/quest"

# The symbol a GOT hook would name, and the slot a pin would carry.
SLOT_NAMES = re.compile(r"vrapi_ShowSystemUI|0x0*36d5280", re.I)


def strip_comments(text: str) -> str:
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    return re.sub(r"//[^\n]*", "", text)


def quest_sources():
    for path in sorted(QUEST.rglob("*")):
        if path.suffix in (".cpp", ".h", ".cc", ".hpp"):
            yield path.relative_to(ROOT).as_posix(), strip_comments(path.read_text())


class NoQuestCodeHooksTheSystemUiSlot(unittest.TestCase):
    def test_no_quest_source_names_the_slot_outside_a_comment(self):
        files = dict(quest_sources())
        self.assertGreater(len(files), 50)  # a sensor that reads nothing watches nothing
        named = sorted(rel for rel, text in files.items() if SLOT_NAMES.search(text))
        self.assertEqual(named, [], "a Quest source hooks or pins vrapi_ShowSystemUI; QUIT must get the game's own type")

    def test_no_build_compiles_a_system_ui_hook(self):
        for rel in ("justfile", "src/quest/CMakeLists.txt", "src/quest/sentinel/CMakeLists.txt"):
            self.assertIsNone(re.search(r"system_ui_hook", (ROOT / rel).read_text()), f"{rel} builds system_ui_hook")


if __name__ == "__main__":
    unittest.main()
