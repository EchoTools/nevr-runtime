"""ADR 0048 decision 9 and 16: the session map pin and the session's group are wired from the game to the core.

The host tests drive the rules (src/core/map_groups.h, the pin's tick over a fake game) and the Quest router with fake transports.
Production_steps.cpp is the one translation unit no host recipe compiles, and the PC half is not built under the Quest-only order,
so the bindings are pinned here by source shape: the Quest login's map report, the router's request before its record limiter,
the PC client's request on a success, and the PC per-frame tick after the module ticks.
"""

import unittest
from pathlib import Path

from tools.tests.test_runtime_lifecycle_invariants import extract_braced_function

ROOT = Path(__file__).resolve().parents[2]


def read(path: str) -> str:
    return (ROOT / path).read_text()


class TheQuestLoginCarriesThePinEntries(unittest.TestCase):
    def test_the_production_build_sets_the_map_report_from_the_pin_core(self):
        body = extract_braced_function(read("src/quest/integration/production_steps.cpp"), "nevr_quest_login::BuildInfo ThisBuild() {")
        self.assertIn("build.map_report = [] { return nevr_map_session_pin::LoginEntries(); };", body)

    def test_the_login_rewrite_puts_the_report_in_the_login_profile(self):
        cpp = read("src/quest/login/login_rewrite.cpp")
        self.assertIn("if (build.map_report) inputs.maps = build.map_report();", cpp)


class TheSessionGroupIsRequestedOnASuccess(unittest.TestCase):
    def test_the_quest_router_requests_the_group_before_its_record_limiter(self):
        body = extract_braced_function(
            read("src/runtime/compat/session_router.cpp"),
            "void Router::LobbyAnswersLocked(const std::string& frame, int connIdx, Effects& fx) {",
        )
        request = body.index("nevr_maps::RequestSessionGroup(nevr_lobby_frame::FormatGuid(success.group), &group)")
        limiter = body.index("clientRecords_.Admit(*kind, now)")
        self.assertLess(request, limiter)

    def test_the_pc_client_requests_the_group_on_a_success_before_its_record_limiter(self):
        body = extract_braced_function(
            read("src/runtime/compat/ws_bridge.cpp"),
            "static void LogClientLobbyMessage(int connIdx, bool fromServer, uint64_t symbol, const uint8_t* payload, uint64_t len) {",
        )
        request = body.index("nevr_maps::RequestSessionGroup(nevr_lobby_frame::FormatGuid(state.group), &group)")
        limiter = body.index("if (!verdict.log) return;")
        self.assertLess(request, limiter)
        self.assertIn("if (!g_isServer) {", body[:request])


class ThePcTickRunsAfterTheModuleTicks(unittest.TestCase):
    def test_the_per_frame_dispatcher_calls_the_pin_after_the_modules(self):
        body = extract_braced_function(read("src/runtime/frame/tick.cpp"), "void DispatchPerFrameWork(uint64_t nowUs) {")
        modules = body.index("TickModules(&mctx);")
        pin = body.index("nevr_map_session_pin::Tick();")
        self.assertLess(modules, pin)

    def test_the_pin_tick_asks_the_game_for_nothing_while_no_map_is_listed(self):
        cpp = read("src/core/map_session_pin.cpp")
        body = extract_braced_function(cpp, "void TickWith(GameAccess& game) {")
        self.assertLess(body.index("g_view.listed.empty()"), body.index("game.Callable()"))


if __name__ == "__main__":
    unittest.main()
