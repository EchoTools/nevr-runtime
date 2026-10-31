"""#1756: a session the service ended releases its server.

Source sensors for what the GTests in src/runtime/tests/test_session_unregister.cpp (EndedSessionExit.*) cannot
reach: gameserver_callbacks.cpp is not linked by any test target, so the wiring around the pure watchdog is read
here.

  - the WebSocket receive observer parses each message with ParseServiceEndedSession and arms the watchdog;
  - the expiry ends the process through PerformGracefulShutdown(0), after a Warning that names the session;
  - the delay adds the configured empty-server hold to the grace;
  - a new session cancels the watchdog.
"""

import unittest
from pathlib import Path

from tools.tests.test_runtime_lifecycle_invariants import extract_braced_function, strip_comments


ROOT = Path(__file__).resolve().parents[2]


def callbacks() -> str:
    return strip_comments((ROOT / "src/runtime/server/gameserver_callbacks.cpp").read_text())


class EndedSessionExitWiring(unittest.TestCase):
    def test_the_receive_observer_arms_on_a_parsed_code_ended(self):
        body = extract_braced_function(callbacks(), "void GameServerLib::RegisterTcpCallbacks()")
        self.assertIn("SetReceiveObserver", body)
        observer = body[body.index("SetReceiveObserver"):]
        observer = observer[: observer.index("SetMessageHandler")]
        self.assertIn("if (!nevr_game_server::ParseServiceEndedSession(", observer)
        self.assertLess(observer.index("ParseServiceEndedSession("), observer.index("return;"))
        self.assertLess(observer.index("return;"), observer.index("ArmEndedSessionWatchdog("))

    def test_the_expiry_ends_the_process_with_a_warning(self):
        body = extract_braced_function(callbacks(), "void GameServerLib::ArmEndedSessionWatchdog(const std::string& sessionId)")
        self.assertIn("m_endedSessionWatchdog.Arm(", body)
        warning = body[body.index("LogLevel::Warning"):body.index("PerformGracefulShutdown(0)")]
        self.assertIn("session_id=%s", warning)
        self.assertIn("running_scripts=", warning)

    def test_the_delay_adds_the_empty_server_hold_to_the_grace(self):
        body = extract_braced_function(callbacks(), "void GameServerLib::ArmEndedSessionWatchdog(const std::string& sessionId)")
        self.assertIn("kEndedSessionExitGrace + std::chrono::milliseconds(nevr_return_to_lobby::HoldTtlMs())", body)

    def test_a_new_session_cancels_the_watchdog(self):
        text = callbacks()
        start = text.index("StartSession()")
        self.assertIn("CancelEndedSessionWatchdog()", text[start:start + 800])


if __name__ == "__main__":
    unittest.main()
