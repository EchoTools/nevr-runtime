"""#752: the login remote's callbacks never read a ProxyPair through the raw pointer they captured.

g_pairs owns each ProxyPair. A game socket that closes erases its pair under g_pairsMutex, and the login
remote's callback can outlive that pair (the login game socket closes while a matchmaker socket still shares
the remote and keeps the callback installed, RetireGameWsLocked). The pointer the callback captured then
dangles, and taking the mutex does not help: the mutex guards the map, not the freed object. So the callback
finds its pair by its game socket, under the mutex, and never names pairPtr.

Source-shape sensors: the GTests drive the bridge through real sockets, and the interleaving needs the erase
to land between the callback's entry and its lock, which no test can hold open. What is pinned here:

  - no `pairPtr->` dereference anywhere in ws_bridge.cpp (comments stripped);
  - both places that read the login pair's remote in the login remote's Message branch (the LoginSuccess
    subscribe send and the routing decision) look the pair up by gameWsPtr inside g_pairsMutex first.
"""

import re
import unittest
from pathlib import Path

from tools.tests.test_runtime_lifecycle_invariants import strip_comments


ROOT = Path(__file__).resolve().parents[2]
BRIDGE = ROOT / "src" / "runtime" / "compat" / "ws_bridge.cpp"

# The lookup the callback makes under g_pairsMutex: the pair found by the game socket, never by pairPtr.
LOOKUP_UNDER_LOCK = re.compile(
    r"lock_guard<std::mutex>\s+\w+\(g_pairsMutex\);\s*(?:const\s+)?auto\s+\w+\s*=\s*g_pairs\.find\(gameWsPtr\)"
)


def bridge_code() -> str:
    return strip_comments(BRIDGE.read_text(encoding="utf-8"))


class LoginRemotePairLifetime(unittest.TestCase):
    def test_no_callback_dereferences_the_captured_pair_pointer(self):
        derefs = re.findall(r"\bpairPtr\s*->", bridge_code())
        self.assertEqual(derefs, [], "pairPtr is a raw ProxyPair* that g_pairs may have freed (#752)")

    def test_the_login_remote_branch_finds_its_pair_by_game_socket_under_the_mutex(self):
        # LoginSuccess subscribe send and the Message routing decision: two lookups in the same callback.
        found = LOOKUP_UNDER_LOCK.findall(bridge_code())
        self.assertGreaterEqual(len(found), 2, "each login-remote Message lookup takes g_pairsMutex first (#752, #738)")

    def test_the_routing_lookup_checks_the_pair_exists_before_it_uses_it(self):
        self.assertRegex(bridge_code(), r"const auto pit = g_pairs\.find\(gameWsPtr\);[\s\S]{0,160}?pit != g_pairs\.end\(\)",
                         "a closed game socket's pair is gone: the routing check tests for it before reading remoteWs")


if __name__ == "__main__":
    unittest.main()
