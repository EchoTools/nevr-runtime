"""#906 / ADR 0017: the card-choice observer is wired once, on a client only, and what it learns reaches the request
plan. The pure logic is tested by src/runtime/tests/test_invite_choices.cpp; these are source sensors for what that
cannot see: the install call and its server-mode log in boot.cpp, the checked prologue and the forwarding in the hook,
and that the click-metric observer plans from the observed choices, never from an empty set. Each check takes the text
it reads, so a test can hand it a broken copy and see it fail."""
from __future__ import annotations

import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
BOOT = ROOT / "src/runtime/lifecycle/boot.cpp"
HOOK = ROOT / "src/runtime/patch/invite_choice_hook.cpp"
METRIC_HOOK = ROOT / "src/runtime/patch/invite_wizard_hook.cpp"

CALL = "nevr_invite_choice_hook::Install(reinterpret_cast<std::uintptr_t>(GetModuleHandleA(nullptr)));"
SERVER_LOG = "[NEVR.INVITE] choice observer skipped reason=server_mode"


def strip_comments(text: str) -> str:
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    return re.sub(r"//[^\n]*", "", text)


def boot_problems(boot: str) -> list[str]:
    code = strip_comments(boot)
    if code.count(CALL) != 1:
        return [f"expected exactly one `{CALL}` in boot.cpp, found {code.count(CALL)}"]
    start = code.index(CALL)
    depth = 0
    open_at = -1
    for i in range(start - 1, -1, -1):
        if code[i] == "}":
            depth += 1
        elif code[i] == "{":
            if depth == 0:
                open_at = i
                break
            depth -= 1
    if open_at < 0 or not code[:open_at].rstrip().endswith("if (!g_isServer)"):
        return ["the Install call is not directly inside an `if (!g_isServer) {` block"]
    after = code[start + len(CALL):]
    close = re.search(r"\}\s*else\s*\{(.*?)\n  \}", after, flags=re.S)
    if not close or SERVER_LOG not in close.group(1):
        return [f"the else branch of the `if (!g_isServer)` block does not log `{SERVER_LOG}`"]
    return []


def hook_problems(hook: str) -> list[str]:
    code = strip_comments(hook)
    problems = []
    if "0x14050fb60ULL" not in code:
        problems.append("the hook does not name SendComponentEvent at 0x14050fb60")
    m = re.search(r"kSendComponentEventPrologue\s*=\s*\{(.*?)\};", code, flags=re.S)
    if not m or len(re.findall(r"0x[0-9A-Fa-f]{2}", m.group(1))) != 35:
        problems.append("the prologue pin is not the 35 bytes before the function's first call")
    if "ValidatePrologue(target, kSendComponentEventPrologue.data(), kSendComponentEventPrologue.size())" not in code:
        problems.append("the target is not checked against the whole prologue pin before it is detoured")
    if code.index("ValidatePrologue") > code.index("PatchDetour("):
        problems.append("the detour is installed before the prologue is checked")
    if "return g_original(space, event, flags);" not in code:
        problems.append("the hook does not forward every event to the game's function")
    if "IsReadableMemory" not in code:
        problems.append("the hook reads game memory without a readable-memory check")
    if "choice observer off reason=prologue_mismatch" not in code:
        problems.append("a prologue mismatch is not logged")
    if "choice observer installed" not in code:
        problems.append("the install is not logged")
    return problems


def metric_problems(metric_hook: str) -> list[str]:
    code = strip_comments(metric_hook)
    problems = []
    if "nevr_invite_choice_hook::Choices().Snapshot()" not in code:
        problems.append("the click-metric observer does not plan from the observed choices")
    if re.search(r"Choices\s+choices\s*\{\s*\}", code):
        problems.append("the click-metric observer plans from an empty choice set")
    return problems


class InviteChoiceHookWiringTest(unittest.TestCase):
    def test_boot_installs_it_on_a_client_only_and_logs_the_server_skip(self):
        self.assertEqual(boot_problems(BOOT.read_text()), [])

    def test_boot_check_fails_on_an_ungated_call(self):
        broken = "void f() {\n  " + CALL + "\n}\n"
        self.assertNotEqual(boot_problems(broken), [])

    def test_boot_check_fails_without_the_server_skip_log(self):
        text = BOOT.read_text().replace(SERVER_LOG, "[NEVR.INVITE] something else")
        self.assertNotEqual(boot_problems(text), [])

    def test_the_hook_checks_its_prologue_and_forwards_every_event(self):
        self.assertEqual(hook_problems(HOOK.read_text()), [])

    def test_hook_check_fails_on_a_short_pin(self):
        text = HOOK.read_text().replace("0x4C, 0x8B, 0x02}", "0x4C, 0x8B}")
        self.assertNotEqual(hook_problems(text), [])

    def test_hook_check_fails_when_the_event_is_not_forwarded(self):
        text = HOOK.read_text().replace("return g_original(space, event, flags);", "return 0;")
        self.assertNotEqual(hook_problems(text), [])

    def test_the_metric_observer_plans_from_the_observed_choices(self):
        self.assertEqual(metric_problems(METRIC_HOOK.read_text()), [])

    def test_metric_check_fails_on_an_empty_choice_set(self):
        text = METRIC_HOOK.read_text().replace(
            "nevr_invite_choice_hook::Choices().Snapshot()", "nevr_invite::Choices{}"
        ).replace("const nevr_invite::Choices choices = nevr_invite::Choices{};", "const nevr_invite::Choices choices{};")
        self.assertNotEqual(metric_problems(text), [])


if __name__ == "__main__":
    unittest.main()
