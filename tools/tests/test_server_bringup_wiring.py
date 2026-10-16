"""#592: the server bring-up fails loudly instead of idling.

Source-order sensors for what the GTests in src/runtime/tests/test_server_bringup.cpp cannot
reach (they drive the pure decisions in lifecycle/server_bringup.h; the game memory reads, the
logs and the exit live in state_machine.cpp and boot.cpp, which no test target links):

  - the two BeginMultiplayer prologues are checked once at boot, on a server, through
    ValidatePrologue (N97), and a mismatch goes to ServerFatal (N102, N120), with the outcome
    logged at Info (N122, N124);
  - the queued call's not-logged-in path fails the server through ServerFatal;
  - the net game's layout and states are named (addresses.h, NetGameState), not written as 2, 3, 8;
  - the comments that explain the bring-up say what the code does.
"""

import re
import unittest
from pathlib import Path

from tools.tests.test_runtime_lifecycle_invariants import extract_braced_function, strip_comments


ROOT = Path(__file__).resolve().parents[2]


def read(path: str) -> str:
    return (ROOT / path).read_text()


def state_machine() -> str:
    return read("src/runtime/lifecycle/state_machine.cpp")


def validate_body() -> str:
    return extract_braced_function(state_machine(), "void ValidateServerBringup()")


def queued_call_body() -> str:
    return extract_braced_function(state_machine(), "VOID BeginMultiplayerAfterLogin(PVOID netGame)")


class BootValidatesThePrologues(unittest.TestCase):
    def test_state_machine_has_no_raw_memcmp_on_the_prologues(self):
        code = strip_comments(state_machine())
        self.assertNotIn("memcmp", code, "the prologues are compared with ValidatePrologue (N97), not memcmp")

    def test_validation_is_one_function_that_uses_validateprologue_and_serverfatal(self):
        body = validate_body()
        self.assertIn("ValidatePrologues(", body)
        self.assertIn("ServerFatal(", body)

    def test_a_mismatch_is_not_a_warning_and_return(self):
        body = validate_body()
        self.assertNotRegex(body, r"LogLevel::Warning[^;]*prologue_mismatch")

    def test_the_outcome_is_logged_at_info(self):
        body = validate_body()
        self.assertRegex(body, r"Log\(\s*EchoVR::LogLevel::Info\s*,\s*\"\[NEVR\.PATCH\] server bring-up prologues")

    def test_the_queueing_path_no_longer_checks_prologues(self):
        body = extract_braced_function(state_machine(), "void QueueBeginMultiplayer(PVOID netGame)")
        self.assertNotIn("Prologue", body)
        self.assertNotIn("prologue_mismatch", body)

    def test_boot_calls_it_once_on_a_server(self):
        source = read("src/runtime/lifecycle/boot.cpp")
        code = strip_comments(source)
        self.assertEqual(code.count("ValidateServerBringup()"), 1)
        at = code.index("ValidateServerBringup()")
        block = code.rfind("if (g_isServer)", 0, at)
        self.assertNotEqual(block, -1, "the check is not under an `if (g_isServer)` block")
        # No closing of that block between the guard and the call.
        between = code[block:at]
        self.assertEqual(between.count("{"), between.count("}") + 1, "the call sits outside the server block")

    def test_boot_calls_it_before_any_state_switch_can_run(self):
        # The call is in the deferred bootstrap, which runs on the first PreprocessCommandLine call,
        # before the net game exists to switch state.
        source = read("src/runtime/lifecycle/boot.cpp")
        body = extract_braced_function(source, "void RunDeferredRuntimeBootstrap(PVOID pGame, const char* trigger)")
        self.assertIn("ValidateServerBringup()", body)

    def test_it_is_declared_for_boot(self):
        self.assertIn("void ValidateServerBringup();", read("src/runtime/lifecycle/state_machine.h"))


class TheQueuedCallFailsTheServer(unittest.TestCase):
    def test_not_logged_in_goes_through_runqueuedbegin_and_serverfatal(self):
        body = queued_call_body()
        self.assertIn("RunQueuedBegin(", body)
        self.assertIn("ServerFatal(", body)
        self.assertNotRegex(body, r"LogLevel::Warning", "the not-logged-in path is a fatal, not a warning and return")


class TheLayoutIsNamed(unittest.TestCase):
    def test_state_machine_reads_the_net_game_through_named_offsets(self):
        code = strip_comments(state_machine())
        for name in ("NETGAME_GAME_OFFSET", "NETGAME_STATE_OFFSET", "GAME_CALL_QUEUE_OFFSET"):
            self.assertIn(name, code)
        self.assertNotRegex(code, r"\)\s*\+\s*8\s*\)", "a bare `+ 8` offset into the net game or the game object")

    def test_the_offsets_are_defined_in_addresses_h(self):
        code = strip_comments(read("src/runtime/hook/addresses.h"))
        for name in ("NETGAME_GAME_OFFSET", "NETGAME_STATE_OFFSET", "GAME_CALL_QUEUE_OFFSET"):
            self.assertRegex(code, r"constexpr\s+uintptr_t\s+" + name + r"\s*=")

    def test_bringup_uses_netgamestate_not_2_and_3(self):
        code = strip_comments(read("src/runtime/lifecycle/server_bringup.h"))
        self.assertNotRegex(code, r"kLoggingIn\s*=\s*2")
        self.assertNotRegex(code, r"kLoggedIn\s*=\s*3")
        self.assertIn("NetGameState::LoggedIn", code)
        self.assertNotIn("static_assert", strip_comments(state_machine()))


class StaleTextIsFixed(unittest.TestCase):
    def test_spectator_stream_comment_does_not_say_it_might_start_the_lobby(self):
        source = read("src/runtime/lifecycle/boot.cpp")
        self.assertNotIn("neither confirmed nor refuted", source)
        self.assertNotRegex(source, r"what lets a server start its lobby")
        self.assertIn("ValidateServerBringup", source)
        hook = " ".join(
            re.sub(r"^\s*//", "", line).strip()
            for line in source[source.index("#519: the -spectatorstream check"):source.index("#496: plugins marked")].splitlines()
        )
        self.assertIn("does not begin multiplayer", hook)
        self.assertIn("state_machine.cpp", hook)

    def test_spectator_stream_sensor_docstring_says_the_same(self):
        text = read("tools/tests/test_spectatorstream_order.py")
        self.assertNotIn("neither confirmed nor refuted", text)

    def test_sysnet_check_comment_does_not_claim_it_stops_multiplayer_from_beginning(self):
        text = read("src/runtime/hook/addresses.h")
        self.assertNotIn("BeginMultiplayer never completes", text)
        self.assertIn("SYSNET_CHECK", text)


if __name__ == "__main__":
    unittest.main()
