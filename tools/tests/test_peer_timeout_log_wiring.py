"""Peer timeout logging: the PC hooks and their call sites (source sensors, rank 3).

The detours run inside echovr.exe, so no test target in this tree drives them. The pure rules are GTest
(src/runtime/tests/peer_timeout_log_test.cpp). These sensors check what the hook file and its callers say:

  - the server hook sits on CBroadcaster::DestroyPeer and writes its Warning only for the communication-timeout reason;
  - the hook reads the peer record with the game's own offsets and forwards every argument to the original;
  - the install checks each prologue before it attaches;
  - boot installs the hook for the run's role; the NetGame state hook reports in-game entry and exit; the frame tick
    checks the no-send threshold.
"""

import unittest
from pathlib import Path

from tools.tests.test_runtime_lifecycle_invariants import strip_comments


ROOT = Path(__file__).resolve().parents[2]


def read(path: str) -> str:
    return strip_comments((ROOT / path).read_text())


HOOK = "src/runtime/patch/peer_timeout_log_hook.cpp"
HEADER = "src/runtime/patch/peer_timeout_log_hook.h"


class ServerHook(unittest.TestCase):
    def test_hook_is_on_destroy_peer_at_the_reconstructed_address(self):
        self.assertIn("nevr_patch_addresses::DESTROY_PEER", read(HOOK))
        self.assertIn("constexpr uintptr_t DESTROY_PEER = 0xF7BC60;", read("src/runtime/hook/addresses.h"))

    def test_every_removal_is_logged_at_its_reasons_level(self):
        body = read(HOOK)
        self.assertIn("LevelForRemovalReason(removal.reason)", body)
        self.assertIn("FormatPeerRemoved(removal)", body)

    def test_the_peer_record_is_read_before_the_original_destroys_it(self):
        body = read(HOOK)
        self.assertLess(body.index("FormatPeerRemoved(removal)"), body.rindex("OriginalDestroyPeer(self, index, reason)"))

    def test_every_argument_is_forwarded(self):
        self.assertIn("OriginalDestroyPeer(self, index, reason);", read(HOOK))

    def test_the_prologue_is_checked_before_attach(self):
        body = read(HOOK)
        self.assertLess(body.index("PrologueMatches(\"DestroyPeer\""), body.index("PatchDetour(&OriginalDestroyPeer"))


class ClientHook(unittest.TestCase):
    def test_send_is_recorded_before_the_original_runs(self):
        body = read(HOOK)
        self.assertLess(body.index("RecordSend(NowMicros())"), body.index("return OriginalBroadcasterSend("))

    def test_the_prologue_is_checked_before_attach(self):
        body = read(HOOK)
        self.assertLess(body.index("PrologueMatches(\"BroadcasterSend\""), body.index("PatchDetour(&OriginalBroadcasterSend"))

    def test_entering_in_game_logs_the_send_state_at_info(self):
        body = read(HOOK)
        self.assertIn("EnterInGame(NowMicros())", body)
        self.assertRegex(body, r'Log\(EchoVR::LogLevel::Info,\s*"%s",\s*nevr_peer_timeout_log::FormatInGameSendState')

    def test_the_no_send_warning_is_at_warning(self):
        self.assertRegex(read(HOOK), r'Log\(EchoVR::LogLevel::Warning,\s*"%s",\s*line\.c_str\(\)\)')


class Callers(unittest.TestCase):
    def test_boot_installs_the_hook_for_the_run_role_outside_the_server_branch(self):
        boot = read("src/runtime/lifecycle/boot.cpp")
        call = "nevr_peer_timeout_hook::Install(reinterpret_cast<std::uintptr_t>(EchoVR::g_GameBaseAddress), g_isServer);"
        self.assertEqual(boot.count(call), 1)
        self.assertLess(boot.index("nevr_coop_ai_trace::Install"), boot.index(call))

    def test_the_netgame_state_hook_reports_in_game(self):
        self.assertIn(
            "nevr_peer_timeout_hook::OnNetGameState(state == EchoVR::NetGameState::InGame);",
            read("src/runtime/lifecycle/state_machine.cpp"),
        )

    def test_the_frame_tick_checks_the_no_send_threshold(self):
        self.assertIn("nevr_peer_timeout_hook::CheckNoSend();", read("src/runtime/frame/tick.cpp"))

    def test_the_sources_are_in_the_runtime_library(self):
        cmake = read("src/runtime/CMakeLists.txt")
        self.assertIn('"patch/peer_timeout_log_hook.cpp"', cmake)
        self.assertIn('"compat/peer_timeout_log.cpp"', cmake)


if __name__ == "__main__":
    unittest.main()
