"""#1276: the main-menu QUIT finishes the game's activity; no Quest code picks a VrApi system UI by number.

libr15's `CVR::ShowSystemUI` (0x1a73990 in the pinned build) is `mov w1, #1` then a tail call through the
`vrapi_ShowSystemUI` JUMP_SLOT (0x36d5280). What a type shows belongs to the system's VrApi and differs by OS: on
Horizon OS v2.7 a hook that sent type 0 there opened the Settings panel, and on a current OS the game's own type 1
opens the Meta library (#392). The APK's libvrapi.so forwards the call through a table, so this tree cannot say what a
type does. The quit hook (src/quest/sentinel/quit_hook.h) takes the game's own `CVR::ShowSystemUI` slot instead and
finishes the activity, so no Quest source names the VrApi slot. Its handler is driven by quit_hook_test.cpp and its
slot is checked against the real libr15 by got_pinned_test.cpp. This file pins what neither can see: that production
builds and installs it, and that nothing hooks the VrApi slot.
"""

import re
import unittest
from pathlib import Path

from tools.tests.test_runtime_lifecycle_invariants import extract_braced_function

ROOT = Path(__file__).resolve().parents[2]
QUEST = ROOT / "src/quest"

# The VrApi symbol a GOT hook would name, and the slot a pin would carry.
VRAPI_SLOT = re.compile(r"vrapi_ShowSystemUI|0x0*36d5280", re.I)


def read(path: str) -> str:
    return (ROOT / path).read_text()


def strip_comments(text: str) -> str:
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    return re.sub(r"//[^\n]*", "", text)


def quest_sources():
    for path in sorted(QUEST.rglob("*")):
        if path.suffix in (".cpp", ".h", ".cc", ".hpp"):
            yield path.relative_to(ROOT).as_posix(), strip_comments(path.read_text())


class NoQuestCodeHooksTheVrApiSlot(unittest.TestCase):
    def test_no_quest_source_names_the_vrapi_slot_outside_a_comment(self):
        files = dict(quest_sources())
        self.assertGreater(len(files), 50)  # a sensor that reads nothing watches nothing
        named = sorted(rel for rel, text in files.items() if VRAPI_SLOT.search(text))
        self.assertEqual(named, [], "a Quest source hooks or pins vrapi_ShowSystemUI; QUIT must not pick a type by number")

    def test_no_build_compiles_the_type_rewriting_hook(self):
        for rel in ("justfile", "src/quest/CMakeLists.txt", "src/quest/sentinel/CMakeLists.txt"):
            self.assertIsNone(re.search(r"system_ui_hook", read(rel)), f"{rel} builds system_ui_hook")


class TheQuitHookIsWired(unittest.TestCase):
    def test_the_slot_is_the_games_own_function(self):
        text = read("src/quest/sentinel/pinned_targets.h")
        self.assertIn('kShowSystemUISymbol = "_ZN10NRadEngine3CVR12ShowSystemUIEv"', text)
        body = extract_braced_function(text, "inline GotTarget LibR15ShowSystemUI() {")
        self.assertRegex(body, r"kLibR15,\s*kShowSystemUISymbol,\s*RelocKind::kJumpSlot,\s*LibR15PatchedBuildId\(\),\s*0x36fb328ULL")

    def test_the_hook_is_built_without_exceptions_into_the_sentinel(self):
        cmake = read("src/quest/sentinel/CMakeLists.txt")
        self.assertRegex(cmake, r"add_library\(ovrplatformloader SHARED[^)]*\bquit_hook\.cpp")
        props = re.search(r"set_source_files_properties\(([^)]*)PROPERTIES COMPILE_OPTIONS \"-fno-exceptions\"\)", cmake)
        self.assertIsNotNone(props)
        self.assertIn("quit_hook.cpp", props.group(1))

    def test_every_launch_installs_it(self):
        body = extract_braced_function(
            strip_comments(read("src/quest/sentinel/entry.cpp")), "bool InstallPermissionWaitHook() noexcept {"
        )
        self.assertIn("nevr_quest::quit::Install();", body)

    def test_the_host_test_runs_in_the_hooks_recipe(self):
        justfile = read("justfile")
        self.assertIn("src/quest/tests/quit_hook_test.cpp", justfile)
        self.assertIn('timeout 120 "$out/quit_hook_test"', justfile)


if __name__ == "__main__":
    unittest.main()
