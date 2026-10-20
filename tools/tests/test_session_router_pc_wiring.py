"""The PC build compiles the session router and runs its host test (#708 S0).

The router (src/runtime/compat/session_router.cpp) is the one place the session rules live; the Quest build
compiles it, and the PC build must compile the same file and run the router's own host test under Wine, or the
PC suites exercise a different rule set. Each check names the line that makes it fail.
"""

import re
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
CMAKE = (REPO / "src/runtime/CMakeLists.txt").read_text(encoding="utf-8")
JUSTFILE = (REPO / "justfile").read_text(encoding="utf-8")


def block(text, start, end):
    i = text.index(start)
    return text[i:text.index(end, i)]


class SessionRouterPcWiringTest(unittest.TestCase):
    def test_the_runtime_dll_compiles_the_router(self):
        sources = block(CMAKE, "set(PATCHES_SOURCES", ")\n")
        self.assertRegex(sources, r'(?m)^\s*"compat/session_router\.cpp"\s*$', "nevr_runtime does not compile the router")

    def test_the_router_host_test_is_built_from_the_quest_test_source(self):
        exe = block(CMAKE, "add_executable(test_session_router", ")\n")
        for src in ("../quest/tests/session_router_test.cpp", "compat/session_router.cpp", "compat/evr_codec.cpp"):
            self.assertIn(src, exe)

    def test_test_auth_unit_builds_and_runs_it(self):
        self.assertIn("--target test_session_router", JUSTFILE)
        loops = [m for m in re.findall(r"for test_name in ([^;]*); do", JUSTFILE) if "test_asset_cache" in m]
        self.assertEqual(len(loops), 1)
        self.assertIn("test_session_router", loops[0].split(), "test-auth-unit never runs test_session_router")


if __name__ == "__main__":
    unittest.main()
