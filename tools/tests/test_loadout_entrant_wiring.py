"""#772: the saved loadout's entrant_id is the client session UUID, never the account id.

The GTests in src/runtime/tests/test_server_context.cpp cover the accessor (ServerContext::
TryGetEntrantSessionGuid). The call site, OnMsgSaveLoadoutRequest in
src/runtime/server/gameserver_callbacks.cpp, reads the net game's memory and no test target
links it, so these source sensors pin what it does with the accessor:

  - the session GUID comes from TryGetEntrantSessionGuid for the slot being saved;
  - a slot with no accepted client session logs at Warning and returns, sending nothing;
  - entrant_id is set once, from that GUID formatted by GuidToUuidString, after the guard;
  - the account id never reaches set_entrant_id;
  - the handler builds one game_server_save_loadout envelope and sends it, and nothing else in
    src/runtime builds one (the game service handles GameServerSaveLoadout since nakama #708).
"""

import re
import unittest
from pathlib import Path

from tools.tests.test_runtime_lifecycle_invariants import extract_braced_function


ROOT = Path(__file__).resolve().parents[2]


def handler_body() -> str:
    source = (ROOT / "src/runtime/server/gameserver_callbacks.cpp").read_text()
    return extract_braced_function(
        source,
        "void OnMsgSaveLoadoutRequest(GameServerLib* self, VOID*, VOID* msg, UINT64 msgSize, EchoVR::Peer, EchoVR::Peer)",
    )


GUARD = re.compile(
    r"GUID\s+entrantSessionGuid\s*\{\s*\}\s*;\s*"
    r"if\s*\(\s*!\s*self->GetContext\(\)\.TryGetEntrantSessionGuid\(\s*playerSlot\s*,\s*&entrantSessionGuid\s*\)\s*\)\s*\{"
    r"(?P<drop>[^{}]*)\}"
)


class SaveLoadoutEntrantId(unittest.TestCase):
    def test_the_session_guid_comes_from_the_accepted_slot_and_a_miss_sends_nothing(self):
        match = GUARD.search(handler_body())
        self.assertIsNotNone(match, "the TryGetEntrantSessionGuid guard on playerSlot is missing or rewritten")
        drop = match.group("drop")
        self.assertRegex(drop, r"Log\(\s*EchoVR::LogLevel::Warning\s*,", "the drop branch is not logged at Warning")
        self.assertRegex(drop, r"\breturn\s*;", "the drop branch does not return, so a message would still be sent")

    def test_entrant_id_is_the_formatted_session_guid_set_after_the_guard(self):
        body = handler_body()
        calls = [m.start() for m in re.finditer(r"set_entrant_id\(", body)]
        self.assertEqual(len(calls), 1, "entrant_id must be set exactly once")
        self.assertRegex(
            body,
            r"saveLoadout->set_entrant_id\(\s*GuidToUuidString\(\s*entrantSessionGuid\s*\)\s*\)\s*;",
        )
        guard = GUARD.search(body)
        self.assertIsNotNone(guard)
        self.assertGreater(calls[0], guard.end(), "entrant_id is set before the guard that can drop the message")

    def test_the_account_id_never_reaches_the_save(self):
        body = handler_body()
        self.assertNotRegex(body, r"set_entrant_id\([^;]*accountId")
        self.assertNotRegex(body, r"set_entrant_id\([^;]*to_string")

    def test_the_handler_sends_one_save_loadout_envelope(self):
        body = handler_body()
        self.assertEqual(len(re.findall(r"mutable_game_server_save_loadout\(", body)), 1)
        self.assertEqual(len(re.findall(r"SendProtobufEnvelope\(\s*self\s*,\s*envelope\s*\)", body)), 1)
        self.assertNotRegex(body, r"not sent: the game service does not accept")

    def test_no_other_runtime_file_builds_a_save_loadout_envelope(self):
        builders = [
            str(path.relative_to(ROOT))
            for path in sorted((ROOT / "src/runtime").rglob("*.cpp"))
            if "mutable_game_server_save_loadout" in path.read_text()
        ]
        self.assertEqual(builders, ["src/runtime/server/gameserver_callbacks.cpp"])


if __name__ == "__main__":
    unittest.main()
