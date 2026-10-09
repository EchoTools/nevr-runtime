import re
import unittest
from pathlib import Path

from tools.tests.test_runtime_lifecycle_invariants import extract_braced_function


ROOT = Path(__file__).resolve().parents[2]


def wrapper_body() -> str:
    source = (ROOT / "src/runtime/lifecycle/crash_recovery.cpp").read_text()
    return extract_braced_function(source, "static VOID GameMainWrapperHook(")


class ServerHoldExits(unittest.TestCase):
    def test_no_unconditional_hold_loop(self):
        """#102: ExitProcess is suppressed in server mode, so a `while (true) { Sleep }` hold in
        the game-main wrapper can never end; a shutdown request after it would hang forever."""
        body = wrapper_body()
        self.assertEqual(re.findall(r"while\s*\(\s*(?:true|1)\s*\)", body), [],
                         "GameMainWrapperHook must not contain an unconditional hold loop")

    def test_game_loop_return_on_a_server_is_the_shutdown(self):
        """#102 (b): after GameMain returns on a server, the wrapper exits through
        PerformGracefulShutdown instead of holding."""
        body = wrapper_body()
        after_loop = body.split("GameMain(arg1);", 1)[1]
        server_tail = after_loop.split("return;", 1)[1]
        self.assertIn("PerformGracefulShutdown(0)", server_tail)

    def test_crash_hold_ends_on_console_shutdown(self):
        """#102 (a): the post-crash hold polls ConsoleShutdownPending() and then exits."""
        body = wrapper_body()
        crash_branch = body.split("GameMain(arg1);", 1)[0]
        self.assertRegex(crash_branch, r"while\s*\(\s*!\s*ConsoleShutdownPending\(\)\s*\)")
        self.assertIn("PerformGracefulShutdown(0)", crash_branch)


if __name__ == "__main__":
    unittest.main()
