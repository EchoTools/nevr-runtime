"""#1857: the press on the PLUGINS card is decided in the wizard observer's path (one detour on SendComponentEvent, never a
second) and written once at Info, into a fixed buffer. SendComponentEvent has about 25 callers (event-driven callers, rate not
measured), so the hook must not do per-call work beyond the cheap read: no per-call log line, no sender walk of its own, no
heap on the path of an event that is not the press. The cost is tested by src/runtime/tests/test_plugins_card.cpp and
test_menu_press.cpp; these are source sensors for what that cannot see: the call in the hook, its level, its buffer, and
the tag row. Each check takes the text it reads, so a test can hand it a broken copy and see it fail."""
from __future__ import annotations

import re
import unittest
from pathlib import Path

TESTS_DIR = Path(__file__).resolve().parent  # <repo>/tools/tests
ROOT = TESTS_DIR.parent.parent  # the repo root
HOOK = ROOT / "src/runtime/patch/invite_choice_hook.cpp"
STANDARD = ROOT / "docs/standards/logging.md"
TAG = "[NEVR.PLUGINMGR]"
# Install and EnsureHook: the one detour on SendComponentEvent is attached from these two sites.
EXPECTED_PATCH_DETOURS = 2


def strip_comments(text: str) -> str:
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    return re.sub(r"//[^\n]*", "", text)


def hook_problems(hook: str) -> list[str]:
    code = strip_comments(hook)
    problems = []
    for header in ("menu_press.h", "plugins_card.h"):
        if f'#include "runtime/patch/{header}"' not in code:
            problems.append(f"the hook does not include patch/{header}")
    call = "nevr_plugins_card::ReadPluginsPress("
    if code.count(call) != 1:
        problems.append(f"expected one {call} call, found {code.count(call)}")
    # The walk and the per-call read belong to ReadPluginsPress, after its cheap head: the hook calls neither itself.
    for forbidden in (
        "nevr_menu_press::ResolveOwner(",
        "nevr_menu_press::ReadHead(",
        "nevr_menu_press::ReadPress(",
        "nevr_menu_press::PressLine(",
        "nevr_invite_choices::ReadSender(",
    ):
        if forbidden in code:
            problems.append(f"the hook calls {forbidden} itself, so it runs for every event")
    if re.search(r"Log\(\s*EchoVR::LogLevel::Debug,[^;]*PLUGINMGR", code):
        problems.append("the hook writes a per-call Debug line under [NEVR.PLUGINMGR]")
    if not re.search(r"char\s+line\[nevr_plugins_card::kPluginsLineBytes\];", code):
        problems.append("the plugins press line is not built into a fixed char buffer")
    if not re.search(r"nevr_plugins_card::PluginsPressLine\(\s*line,\s*sizeof\(line\),\s*press,\s*sender,\s*seen\s*\)", code):
        problems.append("the plugins press line is not written through PluginsPressLine into that buffer with the press, the sender and seen")
    if not re.search(r'Log\(\s*EchoVR::LogLevel::Info,\s*"%s",\s*line\s*\)', code):
        problems.append("the plugins press line is not logged at Info from the buffer")
    # The press block must come first in Observe and be the only thing that builds the line: no std::string before it.
    observe = code.split("void Observe(", 1)[-1]
    before_decision = observe.split(call, 1)[0]
    if "std::string" in before_decision or ".c_str()" in before_decision:
        problems.append("the hook builds a std::string before the press decision, so every event allocates")
    if code.count("PatchDetour(") != EXPECTED_PATCH_DETOURS:
        problems.append("the hook no longer has exactly the two PatchDetour call sites of its one detour (Install and EnsureHook)")
    return problems


def standard_problems(standard: str) -> list[str]:
    row = re.search(r"^\| `\[NEVR\.PLUGINMGR\]`[^\n]*$", standard, flags=re.M)
    if not row:
        return [f"{TAG} has no row in the tag table of docs/standards/logging.md"]
    text = row.group(0)
    problems = []
    if "Info" not in text:
        problems.append(f"the {TAG} row does not say its line is Info")
    if "Debug" in text:
        problems.append(f"the {TAG} row still describes a Debug line, which the hook no longer writes")
    return problems


class MenuPressWiringTest(unittest.TestCase):
    def test_the_hook_decides_the_press_once_and_logs_it_at_info_from_a_fixed_buffer(self):
        self.assertEqual(hook_problems(HOOK.read_text()), [])

    def test_hook_check_fails_when_the_per_call_debug_line_comes_back(self):
        text = HOOK.read_text().replace(
            "  nevr_menu_press::Press press;",
            '  Log(EchoVR::LogLevel::Debug, "[NEVR.PLUGINMGR] component event seen=%llu", static_cast<unsigned long long>(seen));\n'
            "  nevr_menu_press::Press press;",
        )
        self.assertNotEqual(hook_problems(text), [])

    def test_hook_check_fails_when_the_hook_walks_the_sender_itself(self):
        text = HOOK.read_text().replace(
            "  nevr_menu_press::Press press;",
            "  nevr_menu_press::Press walked;\n"
            "  nevr_menu_press::ResolveOwner(reader, static_cast<std::uintptr_t>(space), event, &walked);\n"
            "  nevr_menu_press::Press press;",
        )
        self.assertNotEqual(hook_problems(text), [])

    def test_hook_check_fails_when_the_line_is_a_std_string(self):
        text = HOOK.read_text().replace(
            "    char line[nevr_plugins_card::kPluginsLineBytes];\n",
            "    const std::string line(256, ' ');\n",
        )
        self.assertNotEqual(hook_problems(text), [])

    def test_hook_check_fails_when_a_string_is_built_before_the_decision(self):
        text = HOOK.read_text().replace(
            "  nevr_menu_press::Press press;",
            '  const std::string always = "built for every event";\n  nevr_menu_press::Press press;',
        )
        self.assertNotEqual(hook_problems(text), [])

    def test_hook_check_fails_when_the_press_line_is_logged_at_debug(self):
        text = HOOK.read_text().replace(
            'Log(EchoVR::LogLevel::Info, "%s", line)',
            'Log(EchoVR::LogLevel::Debug, "%s", line)',
        )
        self.assertNotEqual(hook_problems(text), [])

    def test_hook_check_fails_when_the_line_loses_the_call_count(self):
        text = HOOK.read_text().replace("press, sender, seen);", "press, sender, 0);")
        self.assertNotEqual(hook_problems(text), [])

    def test_hook_check_fails_without_the_decision(self):
        text = HOOK.read_text().replace("nevr_plugins_card::ReadPluginsPress(", "nevr_plugins_card::ReadNothing(")
        self.assertNotEqual(hook_problems(text), [])

    def test_the_tag_row_in_the_logging_standard_describes_the_info_line(self):
        self.assertEqual(standard_problems(STANDARD.read_text()), [])

    def test_standard_check_fails_without_the_row(self):
        text = STANDARD.read_text().replace("`[NEVR.PLUGINMGR]`", "`[NEVR.OTHER]`")
        self.assertNotEqual(standard_problems(text), [])

    def test_standard_check_fails_when_the_row_still_says_debug(self):
        text = STANDARD.read_text().replace("`[NEVR.PLUGINMGR]`    | The in-game plugin manager", "`[NEVR.PLUGINMGR]`    | Debug line. The in-game plugin manager")
        self.assertNotEqual(standard_problems(text), [])


if __name__ == "__main__":
    unittest.main()
