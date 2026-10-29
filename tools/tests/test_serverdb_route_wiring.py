"""#1266: a dedicated server with no ServerDB route stops with a cause instead of dialing a default address.

The route choice is a pure function, ChooseServerDbRoute (src/runtime/server/serverdb_uri.h), driven by the GTests
in src/runtime/tests/test_serverdb_uri.cpp. These sensors cover what no test target links: RequestRegistration in
gameserver_serverdb.cpp asks it, logs the decision at Info, and on kNone calls ServerFatal with the cause before
anything dials. No default ServerDB address is compiled into the runtime.
"""

import re
import unittest
from pathlib import Path

from tools.tests.test_runtime_lifecycle_invariants import extract_braced_function, strip_comments


ROOT = Path(__file__).resolve().parents[2]


def request_registration() -> str:
    source = (ROOT / "src/runtime/server/gameserver_serverdb.cpp").read_text()
    return strip_comments(extract_braced_function(source, "VOID GameServerLib::RequestRegistration("))


class RequestRegistrationStopsWithoutARoute(unittest.TestCase):
    def test_the_route_is_chosen_by_the_tested_function(self):
        self.assertTrue("nevr_serverdb_uri::ChooseServerDbRoute(" in request_registration(),
                        "RequestRegistration does not call ChooseServerDbRoute")

    def test_the_decision_is_logged_at_info_with_the_route_name(self):
        body = request_registration()
        for needle in ('"[NEVR.GAMESERVER] serverdb route=%s ', "nevr_serverdb_uri::ServerDbRouteName(route)"):
            with self.subTest(needle=needle):
                self.assertTrue(needle in body, f"RequestRegistration lacks {needle}")

    def test_no_route_stops_the_server_before_anything_dials(self):
        body = request_registration()
        match = re.search(r"if \(route == nevr_serverdb_uri::ServerDbRoute::kNone\) \{\s*"
                          r"ServerFatal\(\"%s\", nevr_serverdb_uri::kNoServerDbUriCause\);\s*return;\s*\}", body)
        self.assertIsNotNone(match, "the kNone branch must ServerFatal with kNoServerDbUriCause and return")
        stop = match.start()
        connect = body.find("m_wsClient->Connect(")
        self.assertNotEqual(connect, -1, "RequestRegistration no longer calls m_wsClient->Connect(")
        self.assertLess(stop, connect, "the stop must come before the connection is made")

    def test_no_default_serverdb_address_is_compiled_in(self):
        for path in sorted((ROOT / "src/runtime").rglob("*.cpp")) + sorted((ROOT / "src/runtime").rglob("*.h")):
            if "tests" in path.relative_to(ROOT / "src/runtime").parts:
                continue
            with self.subTest(path=str(path.relative_to(ROOT))):
                self.assertFalse("localhost:777" in strip_comments(path.read_text(errors="replace")),
                                 "a default ServerDB address (localhost:777) is compiled in")


if __name__ == "__main__":
    unittest.main()
