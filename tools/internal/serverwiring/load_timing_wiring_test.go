package serverwiring

import (
	"regexp"
	"strings"
	"testing"
)

// The load timing records (src/runtime/hook/load_timing_rules.h) take their marks at four call sites that no test target links:
// builtin_filter.cpp (the engine-ready line), state_machine.cpp (the LoggedIn switch), join_hold.cpp and load_timing.cpp
// (the LoadLevel detours) and boot.cpp (the install order). The host test test_load_timing_rules drives the header; these pin the
// call sites, over comment-stripped source, so removing one fails here and not silently in production.

func TestTheFilterHandsEachGameLineToOnGameLine(t *testing.T) {
	t.Parallel()
	text := stripComments(read(t, "src/runtime/log/builtin_filter.cpp"))
	const call = "nevr_load_timing::OnGameLine(buf)"
	if !strings.Contains(text, call) {
		t.Fatalf("builtin_filter.cpp does not call %q: engine_ready_ms would never be recorded", call)
	}
	before(t, text, "evr::AnnotateGameLine(", call)
}

func TestTheStateHookCallsOnLoggedInForTheLoggedInState(t *testing.T) {
	t.Parallel()
	hook := squash(body(t, stripComments(read(t, "src/runtime/lifecycle/state_machine.cpp")), "VOID NetGameSwitchStateHook("))
	want := regexp.MustCompile(`if ?\( ?state == EchoVR::NetGameState::LoggedIn ?\) ?\{? ?nevr_load_timing::OnLoggedIn\(\); ?\}?`)
	if !want.MatchString(hook) {
		t.Errorf("NetGameSwitchStateHook does not call OnLoggedIn when the state is LoggedIn: boot_timing would never be written")
	}
}

func TestTheJoinHoldDetourTimesTheLoad(t *testing.T) {
	t.Parallel()
	detour := squash(body(t, stripComments(read(t, joinHoldSource)), "std::uint64_t __fastcall LoadLevelDetour("))
	const call = "nevr_load_timing::TimedLoad(p2, g_originalLoadLevel, gameSpace, p2, p3)"
	if !strings.Contains(detour, call) {
		t.Errorf("the join hold's LoadLevelDetour does not call %q: with the hold armed no level_load record is written", call)
	}
	if strings.Contains(detour, "= g_originalLoadLevel(") {
		t.Errorf("the join hold's LoadLevelDetour calls the original directly, past the timing wrapper")
	}
}

func TestTheLoadTimingDetourTimesTheLoadWithOneFirstLoadFlag(t *testing.T) {
	t.Parallel()
	text := stripComments(read(t, "src/runtime/hook/load_timing.cpp"))
	detour := squash(body(t, text, "std::uint64_t __fastcall LoadLevelDetour("))
	if !strings.Contains(detour, "TimedLoad(p2, g_originalLoadLevel, gameSpace, p2, p3)") {
		t.Errorf("load_timing.cpp's LoadLevelDetour does not call TimedLoad")
	}
	if !strings.Contains(squash(text), "TimeLevelLoad(clock, LogRecord, g_firstLoad,") {
		t.Errorf("TimedLoad does not pass the process-wide g_firstLoad to the wrapper: first_load would be wrong")
	}
}

func TestTheJoinHoldSaysWhetherItsDetourIsOnLoadLevel(t *testing.T) {
	t.Parallel()
	text := squash(stripComments(read(t, joinHoldSource)))
	if !strings.Contains(text, "bool HooksLoadLevel() { return g_loadLevelHooked.load(") {
		t.Errorf("HooksLoadLevel must read g_loadLevelHooked (a join-hold detour is on the address), not g_active (armed)")
	}
	attach := squash(body(t, stripComments(read(t, joinHoldSource)), "bool Attach(std::size_t site)"))
	if !strings.Contains(attach, "g_loadLevelHooked.store(true") {
		t.Errorf("Attach does not record that its LoadLevel detour is installed: the timing hook could put a second detour on the address")
	}
	retire := squash(body(t, stripComments(read(t, joinHoldSource)), "void Retire(std::size_t site)"))
	if !strings.Contains(retire, "g_loadLevelHooked.store(false") {
		t.Errorf("Retire does not clear g_loadLevelHooked when the detour is gone")
	}
}

func TestTheLoadTimingInstallSkipsWhenTheJoinHoldHooksTheAddress(t *testing.T) {
	t.Parallel()
	text := stripComments(read(t, "src/runtime/hook/load_timing.cpp"))
	install := squash(body(t, text, "void Install(bool joinHoldHooksIt)"))
	if !regexp.MustCompile(`if \(joinHoldHooksIt\) \{.*?return; \}`).MatchString(install) {
		t.Errorf("Install does not return early when the join hold hooks LoadLevel: two detours would target one address")
	}
	if strings.Count(install, `LogNotArmed("`) != 2 ||
		!strings.Contains(install, `LogNotArmed("prologue_mismatch", address)`) ||
		!strings.Contains(install, `LogNotArmed("detour_failed", address)`) {
		t.Errorf("Install must warn through LogNotArmed on both the prologue mismatch and the failed detour")
	}
}

func TestTheLoadTimingClocks(t *testing.T) {
	t.Parallel()
	text := squash(stripComments(read(t, "src/runtime/hook/load_timing.cpp")))
	if !strings.Contains(text, "std::chrono::steady_clock::now()") || strings.Contains(text, "GetTickCount64") {
		t.Errorf("the load clock must be steady_clock (QueryPerformanceCounter), not GetTickCount64 (10 to 16 ms steps)")
	}
	elapsed := squash(body(t, stripComments(read(t, "src/runtime/hook/load_timing.cpp")), "std::int64_t ElapsedSinceProcessStartMs()"))
	if !strings.Contains(elapsed, "GetProcessTimes(") || !strings.Contains(elapsed, "return nevr_load_timing::kUnknownMsValue;") {
		t.Errorf("ElapsedSinceProcessStartMs must read GetProcessTimes and return kUnknownMsValue when it fails")
	}
	logged := squash(body(t, stripComments(read(t, "src/runtime/hook/load_timing.cpp")), "void OnLoggedIn()"))
	if !strings.Contains(logged, "MarkLoggedIn(ElapsedSinceProcessStartMs(), &line)") {
		t.Errorf("OnLoggedIn must mark logged-in with the elapsed time since process start")
	}
}

func TestTheLoadLevelPrologueHasOneDefinition(t *testing.T) {
	t.Parallel()
	hook := stripComments(read(t, "src/runtime/hook/load_timing.cpp"))
	hold := stripComments(read(t, joinHoldSource))
	if strings.Contains(hook, "kLoadLevelPrologue =") || strings.Contains(hold, "kLoadLevelPrologue =") {
		t.Errorf("kLoadLevelPrologue is defined in a .cpp: the join hold and the timing hook must share the one in load_timing.h")
	}
	if !strings.Contains(stripComments(read(t, "src/runtime/hook/load_timing.h")), "kLoadLevelPrologue =") {
		t.Errorf("load_timing.h does not define kLoadLevelPrologue")
	}
	if strings.Contains(hold, "std::string HexBytes(") {
		t.Errorf("join_hold.cpp has its own HexBytes: use nevr_load_timing::HexBytes")
	}
}

func TestTheFilterSkipsItsOwnLinesForOnGameLine(t *testing.T) {
	t.Parallel()
	text := squash(stripComments(read(t, "src/runtime/log/builtin_filter.cpp")))
	if !strings.Contains(text, `if (!IsNevrLineOfLength(buf, len)) { nevr_load_timing::OnGameLine(buf); }`) {
		t.Errorf("OnGameLine must run only for lines that do not start with [NEVR.: a runtime line quoting the marker would set engine_ready_ms")
	}
}

func TestTheLoggingStandardNamesEveryLoadTimingLine(t *testing.T) {
	t.Parallel()
	doc := read(t, "docs/standards/logging.md")
	for _, want := range []string{"`load timing armed`", "`load timing hook not installed`"} {
		if !strings.Contains(doc, want) {
			t.Errorf("logging.md's [NEVR.BOOT] row does not name %s", want)
		}
	}
}

func TestLoadTimingInstallFollowsTheJoinHoldInstallWithItsAnswer(t *testing.T) {
	t.Parallel()
	boot := stripComments(read(t, "src/runtime/lifecycle/boot.cpp"))
	const install = "nevr_load_timing::Install(nevr_join_hold::HooksLoadLevel())"
	if !strings.Contains(boot, install) {
		t.Fatalf("boot.cpp does not call %q", install)
	}
	before(t, boot, "nevr_join_hold::Install(", install)
}

func TestTheLoadTimingSourceAndTestsAreBuilt(t *testing.T) {
	t.Parallel()
	cmake := read(t, "src/runtime/CMakeLists.txt")
	for _, want := range []string{`"hook/load_timing.cpp"`, "add_executable(test_load_timing_rules tests/load_timing_rules_test.cpp)"} {
		if !strings.Contains(cmake, want) {
			t.Errorf("src/runtime/CMakeLists.txt does not contain %q", want)
		}
	}
	quest := regexp.MustCompile(`nevr_ht_test\(load_timing_rules_test GROUP shared[^)]*src/quest/tests/load_timing_rules_test\.cpp`)
	if !quest.MatchString(read(t, "src/quest/host_tests/groups/shared.cmake")) {
		t.Errorf("the Quest host build does not compile load_timing_rules_test")
	}
	if n := strings.Count(read(t, "justfile"), "test_load_timing_rules"); n < 2 {
		t.Errorf("justfile names test_load_timing_rules %d times, want its build target and its run (test-auth-unit)", n)
	}
}

func TestTheEngineReadyMarkerIsTheTextARealBootWrote(t *testing.T) {
	t.Parallel()
	header := stripComments(read(t, "src/runtime/hook/load_timing_rules.h"))
	m := regexp.MustCompile(`kEngineReadyMarker\[\] = "([^"]+)"`).FindStringSubmatch(header)
	if m == nil {
		t.Fatalf("load_timing_rules.h defines no kEngineReadyMarker")
	}
	boot := read(t, "tools/tests/fixtures/winvm/healthy_boot.txt")
	if !strings.Contains(boot, m[1]) {
		t.Errorf("kEngineReadyMarker %q is not in a real boot log (tools/tests/fixtures/winvm/healthy_boot.txt): engine_ready_ms would be -1 on every run", m[1])
	}
}
