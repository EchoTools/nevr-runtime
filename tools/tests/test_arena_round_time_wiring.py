"""#982: the public Arena round default is decided from a session type stored before the game starts the session.

Source sensors for what the GTests in src/runtime/tests/test_arena_rules_hook.cpp and test_server_bots.cpp cannot
reach (the callbacks in gameserver_callbacks.cpp are not linked by any test target):

  - the legacy GameServerSessionStart branch stores the frame's game type BEFORE it forwards the frame to the game,
    so the game's reads that the forward triggers see the session's type, not the previous one's or none;
  - the protobuf session create stores through the same function, which logs the type, its source, the type it
    replaces and the reads that found none;
  - the detour counts a read that found no type and exists once, in arena_rules_hook.cpp.
"""

import unittest
from pathlib import Path

from tools.tests.test_runtime_lifecycle_invariants import strip_comments


ROOT = Path(__file__).resolve().parents[2]


def read(path: str) -> str:
    return strip_comments((ROOT / path).read_text())


class TheSessionTypeIsStoredBeforeTheGameStartsTheSession(unittest.TestCase):
    def setUp(self):
        self.callbacks = read("src/runtime/server/gameserver_callbacks.cpp")

    def test_the_legacy_branch_stores_the_frames_type_before_the_forward(self):
        branch = self.callbacks.index("msgId == SYM_LEGACY_SESSION_START")
        store = self.callbacks.index("GameTypeOfSessionStart(", branch)
        forward = self.callbacks.index("Sym::LobbyStartSessionV4", branch)
        self.assertLess(store, forward, "the type is stored after the frame reached the game")
        self.assertIn('"legacy_session_start"', self.callbacks[store:forward])

    def test_the_protobuf_create_stores_through_the_same_function(self):
        create = self.callbacks.index("case gameservice::v1::Envelope::kLobbySessionCreate")
        end = self.callbacks.index("case gameservice::v1::Envelope::kLobbySessionEvent", create)
        self.assertIn('StoreSessionGameType(gameType.gameType, "session_create"', self.callbacks[create:end])
        self.assertNotIn("SetSessionGameType(", self.callbacks[create:end], "a store that skips the log line")

    def test_only_the_store_function_sets_the_type_and_takes_the_count(self):
        self.assertEqual(self.callbacks.count("nevr_arena_round_time::SetSessionGameType("), 1)
        self.assertEqual(self.callbacks.count("nevr_arena_round_time::TakeReadsWithoutType()"), 1)


class TheDetourCountsAReadThatFoundNoType(unittest.TestCase):
    def test_the_count_is_noted_where_the_decision_is_made(self):
        hook = read("src/runtime/lifecycle/arena_rules_hook.cpp")
        self.assertIn("if (!gameType) nevr_arena_round_time::NoteReadWithoutType();", hook)
        self.assertLess(hook.index("DecideRoundTimeDefault(gameType)"), hook.index("NoteReadWithoutType()"))

    def test_the_detour_is_defined_once(self):
        for path in ("src/runtime/lifecycle/config.cpp",):
            self.assertNotIn("FLOAT CJsonGetFloatHook(", read(path))
        self.assertIn("FLOAT CJsonGetFloatHook(", read("src/runtime/lifecycle/arena_rules_hook.cpp"))


if __name__ == "__main__":
    unittest.main()
