"""#730: every runtime call of the game's return to lobby goes through the queue check.

The GTests in src/runtime/tests/test_return_to_lobby_queue.cpp drive the pure check
(lifecycle/return_to_lobby_queue.h). What they cannot reach is the wiring: that no runtime file
calls the game's NetGameScheduleReturnToLobby around it, and that a refusal is logged.
"""

import unittest
from pathlib import Path

from tools.tests.test_runtime_lifecycle_invariants import extract_braced_function, strip_comments


ROOT = Path(__file__).resolve().parents[2]


def read(path: str) -> str:
    return (ROOT / path).read_text()


class EveryCallGoesThroughScheduleNow(unittest.TestCase):
    def test_only_schedule_now_calls_the_game_function(self):
        callers = []
        for path in sorted((ROOT / "src/runtime").rglob("*.cpp")):
            if "tests" in path.parts:
                continue
            code = strip_comments(path.read_text())
            if "NetGameScheduleReturnToLobby(" in code:
                callers.append(path.relative_to(ROOT).as_posix())
        self.assertEqual(callers, ["src/runtime/lifecycle/return_to_lobby.cpp"],
                         "a call of the game's return to lobby outside ScheduleNow skips the queue check (#730)")

    def test_schedule_now_checks_before_it_calls(self):
        body = strip_comments(extract_braced_function(
            read("src/runtime/lifecycle/return_to_lobby.cpp"),
            "bool ScheduleNow(PVOID pGame, const char* caller)"))
        self.assertLess(body.index("nevr_return_to_lobby_queue::Check("),
                        body.index("EchoVR::NetGameScheduleReturnToLobby("))

    def test_a_refusal_is_logged_at_warning_with_its_caller_and_reason(self):
        body = strip_comments(extract_braced_function(
            read("src/runtime/lifecycle/return_to_lobby.cpp"),
            "bool ScheduleNow(PVOID pGame, const char* caller)"))
        self.assertIn("LogLevel::Warning", body)
        self.assertIn("return to lobby not scheduled caller=%s reason=%s", body)

    def test_the_three_runtime_callers_use_schedule_now(self):
        for path, needle in (
            ("src/runtime/lifecycle/state_machine.cpp", 'ScheduleNow(pGame, "load_failed")'),
            ("src/runtime/lifecycle/initialize.cpp", 'ScheduleNow(g_pGame, "export")'),
            ("src/runtime/lifecycle/return_to_lobby.cpp", 'CallGame(pGame, "request")'),
        ):
            self.assertIn(needle, strip_comments(read(path)), path)


if __name__ == "__main__":
    unittest.main()
