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


def routing_block(code: str) -> str:
    """The routing decision in the login remote's Message branch: from the pair lookup to the drop check."""
    start = code.index("const auto pit = g_pairs.find(gameWsPtr);")
    return code[start:code.index("if (target == nullptr)", start)]


FOUND_BRANCH = "if (pit != g_pairs.end()) {"
ERASED_BRANCH = "} else if (remoteAddress == g_loginRemoteWs.get()) {"


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

    def test_gamewsptr_is_used_only_while_its_pair_is_present(self):
        block = routing_block(bridge_code())
        self.assertEqual(block.count("target = gameWsPtr;"), 1, "the one send to the game socket")
        self.assertIn(FOUND_BRANCH, block)
        self.assertIn(ERASED_BRANCH, block)
        send = block.index("target = gameWsPtr;")
        self.assertLess(block.index(FOUND_BRANCH), send, "the game-socket route is inside the found-pair branch")
        self.assertLess(send, block.index(ERASED_BRANCH), "no game-socket route after the erased-pair branch starts")

    def test_an_erased_pair_routes_a_shared_login_remote_and_drops_any_other(self):
        block = routing_block(bridge_code())
        erased = block[block.index(ERASED_BRANCH):]
        # Erased pair, remote still the login remote: the matchmaker route, and nothing else in that branch.
        self.assertRegex(erased, r"^\} else if \(remoteAddress == g_loginRemoteWs\.get\(\)\) \{\s*SharedRouteLocked\(&target\);\s*\}")
        # Erased pair, any other remote: the chain has no else after the login-remote case, so nothing sets target
        # and it stays null into the drop check.
        self.assertNotIn("else", erased[len(ERASED_BRANCH):])
        self.assertNotIn("gameWsPtr", erased)
        self.assertIn("if (target == nullptr)", bridge_code())


if __name__ == "__main__":
    unittest.main()
