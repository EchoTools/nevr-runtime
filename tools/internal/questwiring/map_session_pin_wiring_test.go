package questwiring

import (
	"strings"
	"testing"
)

// ADR 0048 decision 9 and 16: the session map pin and the session's group are wired from the game to the core. The host tests
// drive the rules (src/core/map_groups.h, the pin's tick over a fake game) and the Quest router with fake transports.
// production_steps.cpp is the one translation unit no host recipe compiles, and the PC half is not built under the Quest-only
// order, so the bindings are pinned here by source shape: the Quest login's map report, the router's request before its record
// limiter, the PC client's request on a success, and the PC per-frame tick after the module ticks.

func TestTheProductionBuildSetsTheMapReportFromThePinCore(t *testing.T) {
	b := body(t, stripComments(read(t, "src/quest/integration/production_steps.cpp")), "nevr_quest_login::BuildInfo ThisBuild() {")
	want := "build.map_report = [] { return nevr_map_session_pin::LoginEntries(); };"
	if !strings.Contains(b, want) {
		t.Errorf("ThisBuild() does not contain %q", want)
	}
}

func TestTheLoginRewritePutsTheReportInTheLoginProfile(t *testing.T) {
	cpp := read(t, "src/quest/login/login_rewrite.cpp")
	want := "if (build.map_report) inputs.maps = build.map_report();"
	if !strings.Contains(cpp, want) {
		t.Errorf("login_rewrite.cpp does not contain %q", want)
	}
}

func TestTheQuestRouterRequestsTheGroupBeforeItsRecordLimiter(t *testing.T) {
	b := body(t, stripComments(read(t, "src/runtime/compat/session_router.cpp")),
		"void Router::LobbyAnswersLocked(const std::string& frame, int connIdx, Effects& fx) {")
	before(t, b, "nevr_maps::RequestSessionGroup(nevr_lobby_frame::FormatGuid(success.group), &group)", "clientRecords_.Admit(*kind, now)")
}

func TestThePcClientRequestsTheGroupOnASuccessBeforeItsRecordLimiter(t *testing.T) {
	b := body(t, stripComments(read(t, "src/runtime/compat/ws_bridge.cpp")),
		"static void LogClientLobbyMessage(int connIdx, bool fromServer, uint64_t symbol, const uint8_t* payload, uint64_t len) {")
	const request = "nevr_maps::RequestSessionGroup(nevr_lobby_frame::FormatGuid(state.group), &group)"
	before(t, b, request, "if (!verdict.log) return;")
	if at := strings.Index(b, request); !strings.Contains(b[:at], "if (!g_isServer) {") {
		t.Errorf("the group request is not inside an if (!g_isServer) block")
	}
}

func TestThePerFrameDispatcherCallsThePinAfterTheModules(t *testing.T) {
	b := body(t, stripComments(read(t, "src/runtime/frame/tick.cpp")), "void DispatchPerFrameWork(uint64_t nowUs) {")
	before(t, b, "TickModules(&mctx);", "nevr_map_session_pin::Tick();")
}

func TestThePinTickAsksTheGameForNothingWhileNoMapIsListed(t *testing.T) {
	b := body(t, stripComments(read(t, "src/core/map_session_pin.cpp")), "void TickWith(GameAccess& game) {")
	before(t, b, "g_view().listed.empty()", "game.Callable()")
}
