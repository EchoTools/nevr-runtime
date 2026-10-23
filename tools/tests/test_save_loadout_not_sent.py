"""#832: the game server does not send GameServerSaveLoadout to a game service that cannot take it.

The production game service has no case for `Envelope_GameServerSaveLoadout` in its protobuf
dispatch: the unhandled message is an error and the service drops the game server's connection,
which ends the match. `OnMsgSaveLoadoutRequest` reads raw game memory at fixed offsets, so no
GTest can drive it; these are source pins over its comment-stripped body, the way
`test_server_bringup_wiring.py` pins state_machine.cpp:

  - the handler builds no `game_server_save_loadout` envelope and calls no send;
  - nothing else in src/runtime builds one either;
  - every save request logs one Info line saying it was not sent.
"""

import re
import unittest
from pathlib import Path

from tools.tests.test_runtime_lifecycle_invariants import extract_braced_function, strip_comments


ROOT = Path(__file__).resolve().parents[2]
CALLBACKS = ROOT / "src/runtime/server/gameserver_callbacks.cpp"
SIGNATURE = "void OnMsgSaveLoadoutRequest(GameServerLib* self, VOID*, VOID* msg, UINT64 msgSize, EchoVR::Peer, EchoVR::Peer)"
SKIP_LINE = (
    '"[NEVR.GAMESERVER] [SAVE_LOADOUT] not sent: the game service does not accept this message yet (#772) slot=%u"'
)


def handler_body() -> str:
    return extract_braced_function(CALLBACKS.read_text(), SIGNATURE)


class SaveLoadoutIsNotSent(unittest.TestCase):
    def test_the_handler_builds_no_save_loadout_envelope(self):
        body = handler_body()
        self.assertNotIn("game_server_save_loadout", body)
        self.assertNotIn("gameservice::v1::Envelope", body)

    def test_the_handler_sends_nothing(self):
        body = handler_body()
        self.assertNotRegex(body, r"\bSendProtobufEnvelope\b")
        self.assertNotRegex(body, r"\bSend\w*\(")

    def test_no_runtime_source_builds_the_envelope(self):
        offenders = []
        for path in sorted((ROOT / "src/runtime").rglob("*.cpp")) + sorted((ROOT / "src/runtime").rglob("*.h")):
            if "tests" in path.relative_to(ROOT).parts:
                continue
            if "mutable_game_server_save_loadout" in strip_comments(path.read_text(errors="replace")):
                offenders.append(str(path.relative_to(ROOT)))
        self.assertEqual(offenders, [])

    def test_each_save_request_logs_one_info_line_that_it_was_not_sent(self):
        body = handler_body()
        pattern = r"Log\(\s*EchoVR::LogLevel::Info\s*,\s*" + re.escape(SKIP_LINE) + r"\s*,\s*slot\.slot\s*\)"
        self.assertEqual(len(re.findall(pattern, body)), 1, "expected exactly one skip line at Info, with slot.slot")

    def test_the_skip_line_comes_after_the_slot_check_and_before_any_game_memory_read(self):
        body = handler_body()
        skip = body.index(SKIP_LINE)
        self.assertGreater(skip, body.index("slot.slot >= MAX_PLAYER_SLOTS"))
        self.assertLess(skip, body.index("g_GameBaseAddress"))


if __name__ == "__main__":
    unittest.main()
