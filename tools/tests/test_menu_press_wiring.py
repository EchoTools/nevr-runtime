"""#1857: the press log is emitted from the wizard observer's path (one detour on SendComponentEvent, never a second), at
Debug, the press on the PLUGINS card is decided there and written once at Info, and the tag is in the logging standard. The line and the memory walk are tested by
src/runtime/tests/test_menu_press.cpp; these are source sensors for what that cannot see: the call in the hook, its level,
and the tag row. Each check takes the text it reads, so a test can hand it a broken copy and see it fail."""
from __future__ import annotations

import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
HOOK = ROOT / "src/runtime/patch/invite_choice_hook.cpp"
STANDARD = ROOT / "docs/standards/logging.md"
TAG = "[NEVR.PLUGINMGR]"


def strip_comments(text: str) -> str:
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    return re.sub(r"//[^\n]*", "", text)


def hook_problems(hook: str) -> list[str]:
    code = strip_comments(hook)
    problems = []
    if '#include "runtime/patch/menu_press.h"' not in code:
        problems.append("the hook does not include patch/menu_press.h")
    if code.count("nevr_menu_press::ReadPress(") != 1:
        problems.append(f"expected one nevr_menu_press::ReadPress call, found {code.count('nevr_menu_press::ReadPress(')}")
    if not re.search(r'Log\(\s*EchoVR::LogLevel::Debug,\s*"%s",\s*nevr_menu_press::PressLine\(', code):
        problems.append("the press line is not logged at Debug through nevr_menu_press::PressLine")
    if '#include "runtime/patch/plugins_card.h"' not in code:
        problems.append("the hook does not include patch/plugins_card.h")
    if code.count("nevr_plugins_card::IsPluginsPress(") != 1:
        problems.append(
            f"expected one nevr_plugins_card::IsPluginsPress call, found {code.count('nevr_plugins_card::IsPluginsPress(')}"
        )
    if not re.search(r'Log\(\s*EchoVR::LogLevel::Info,\s*"%s",\s*nevr_plugins_card::PluginsPressLine\(', code):
        problems.append("the plugins press line is not logged at Info through nevr_plugins_card::PluginsPressLine")
    if code.count("PatchDetour(") != 2:
        problems.append("the hook no longer has exactly the two PatchDetour call sites of its one detour (Install and EnsureHook)")
    return problems


def standard_problems(standard: str) -> list[str]:
    row = re.search(r"^\| `\[NEVR\.PLUGINMGR\]`[^\n]*$", standard, flags=re.M)
    if not row:
        return [f"{TAG} has no row in the tag table of docs/standards/logging.md"]
    if "Debug" not in row.group(0):
        return [f"the {TAG} row does not say its line is Debug"]
    return []


class MenuPressWiringTest(unittest.TestCase):
    def test_the_hook_logs_the_press_line_at_debug_from_its_one_detour(self):
        self.assertEqual(hook_problems(HOOK.read_text()), [])

    def test_hook_check_fails_when_the_line_is_logged_above_debug(self):
        text = HOOK.read_text().replace(
            'Log(EchoVR::LogLevel::Debug, "%s", nevr_menu_press::PressLine(',
            'Log(EchoVR::LogLevel::Info, "%s", nevr_menu_press::PressLine(',
        )
        self.assertNotEqual(hook_problems(text), [])

    def test_hook_check_fails_when_the_press_line_is_logged_at_debug(self):
        text = HOOK.read_text().replace(
            'Log(EchoVR::LogLevel::Info, "%s", nevr_plugins_card::PluginsPressLine(',
            'Log(EchoVR::LogLevel::Debug, "%s", nevr_plugins_card::PluginsPressLine(',
        )
        self.assertNotEqual(hook_problems(text), [])

    def test_hook_check_fails_without_the_decision(self):
        text = HOOK.read_text().replace("nevr_plugins_card::IsPluginsPress(", "nevr_plugins_card::IsNothing(")
        self.assertNotEqual(hook_problems(text), [])

    def test_hook_check_fails_without_the_read(self):
        text = HOOK.read_text().replace("nevr_menu_press::ReadPress(", "nevr_menu_press::ReadNothing(")
        self.assertNotEqual(hook_problems(text), [])

    def test_the_tag_is_in_the_logging_standard(self):
        self.assertEqual(standard_problems(STANDARD.read_text()), [])

    def test_standard_check_fails_without_the_row(self):
        text = STANDARD.read_text().replace("`[NEVR.PLUGINMGR]`", "`[NEVR.OTHER]`")
        self.assertNotEqual(standard_problems(text), [])


if __name__ == "__main__":
    unittest.main()
