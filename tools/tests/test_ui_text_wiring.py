"""#896 / #906: the UI text writer is wired once, on a client only, and runs on the frame pacer's thread. The decisions
are tested by src/runtime/tests/test_ui_text.cpp; these are source sensors for what that cannot see: the install call in
boot.cpp with its mode gate and the log for the other mode, the dispatcher's thread gate, the pacer hook that names the
thread, the three pins, and that a text never reaches the log. Each check takes the text it reads, so a test can hand
it a broken copy and see it fail."""
from __future__ import annotations

import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
BOOT = ROOT / "src/runtime/lifecycle/boot.cpp"
TICK = ROOT / "src/runtime/frame/tick.cpp"
PACER = ROOT / "src/runtime/patch/binary_bug_fixes.cpp"
WRITER = ROOT / "src/runtime/patch/ui_text.cpp"
SCENARIO = ROOT / "src/runtime/scenario/scenario_control.cpp"

INSTALL_CALL = "nevr_ui_text::Install(reinterpret_cast<std::uintptr_t>(GetModuleHandleA(nullptr)));"
SERVER_LOG = "[NEVR.UI] text writer skipped reason=server_mode"


def strip_comments(text: str) -> str:
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    return re.sub(r"//[^\n]*", "", text)


def install_problems(boot: str) -> list[str]:
    code = strip_comments(boot)
    if code.count(INSTALL_CALL) != 1:
        return [f"expected exactly one `{INSTALL_CALL}` in boot.cpp, found {code.count(INSTALL_CALL)}"]
    at = code.index(INSTALL_CALL)
    if not code[:at].rstrip().endswith("if (!g_isServer) {"):
        return ["the writer's Install is not directly inside an `if (!g_isServer) {` block"]
    if not re.match(r"\s*\}\s*else\s*\{[^}]*" + re.escape(SERVER_LOG), code[at + len(INSTALL_CALL):]):
        return ["the server branch does not log the skip (N122/N124)"]
    return []


def dispatcher_problems(tick: str) -> list[str]:
    code = strip_comments(tick)
    if "if (OnPacerThread()) nevr_ui_text::OnFrame();" not in code:
        return ["DispatchPerFrameWork does not gate nevr_ui_text::OnFrame on OnPacerThread()"]
    return []


def pacer_problems(pacer: str) -> list[str]:
    code = strip_comments(pacer)
    body = code[code.index("PrecisionSleepWaitHook(") :]
    body = body[: body.index("DispatchPerFrameWork(")]
    if "nevr_frame::NotePacerThread();" not in body:
        return ["PrecisionSleepWaitHook does not name the pacer thread before it dispatches"]
    return []


def writer_problems(writer: str) -> list[str]:
    code = strip_comments(writer)
    problems = []
    for va in ("0x1404F35E0", "0x14091DAF0"):
        if va not in code:
            problems.append(f"the writer does not pin {va}")
    if "kSetTextPrologue" not in code:
        problems.append("the writer does not pin CCanvasUICS::SetText")
    if re.search(r"Log\([^;]*(request\.text|\.text\.c_str\(\))", code):
        problems.append("a log call carries the request's text")
    return problems


class UiTextWiring(unittest.TestCase):
    def test_install_is_client_only_and_logs_the_server_skip(self) -> None:
        self.assertEqual(install_problems(BOOT.read_text()), [])

    def test_a_broken_install_is_caught(self) -> None:
        text = BOOT.read_text().replace("if (!g_isServer) {\n    nevr_ui_text::Install", "{\n    nevr_ui_text::Install")
        self.assertNotEqual(install_problems(text), [])
        self.assertNotEqual(install_problems(BOOT.read_text().replace(SERVER_LOG, "x")), [])

    def test_the_dispatcher_gates_the_writer_on_the_pacer_thread(self) -> None:
        self.assertEqual(dispatcher_problems(TICK.read_text()), [])
        self.assertNotEqual(dispatcher_problems(TICK.read_text().replace("if (OnPacerThread()) ", "")), [])

    def test_the_pacer_hook_names_its_thread(self) -> None:
        self.assertEqual(pacer_problems(PACER.read_text()), [])
        self.assertNotEqual(pacer_problems(PACER.read_text().replace("nevr_frame::NotePacerThread();", "")), [])

    def test_the_writer_pins_its_functions_and_never_logs_a_text(self) -> None:
        self.assertEqual(writer_problems(WRITER.read_text()), [])
        self.assertNotEqual(writer_problems(WRITER.read_text().replace("0x14091DAF0", "0")), [])
        leak = WRITER.read_text() + '\nvoid f(){ Log(EchoVR::LogLevel::Info, "%s", request.text.c_str()); }\n'
        self.assertNotEqual(writer_problems(leak), [])

    def test_the_scenario_action_refuses_while_the_writer_is_off(self) -> None:
        code = strip_comments(SCENARIO.read_text())
        self.assertIn("if (!nevr_ui_text::Ready()) return", code)


if __name__ == "__main__":
    unittest.main()
