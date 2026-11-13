package questwiring

import (
	"regexp"
	"strings"
	"testing"
)

// #2398: the whole-game launch's first-run download and mount are wired where the data step reaches them.
//
// src/quest/tests/whole_game_launch_test.cpp drives the data step with the worker the sentinel sets
// (nevr_quest::cargo::MakeProductionFirstRunWorker). The line that sets it lives in src/quest/sentinel/entry.cpp, which no host test
// links, so it is checked here over comment-stripped source:
//
//   - InstallPermissionWaitHook sets options.all_assets_ready, from the production worker over the configuration's assets, and does so
//     before data_wiring::Install hands the options over;
//   - the mode and the worker are set from the same configuration (ActiveConfig().assets);
//   - the background fetch is not started in the whole-game mode;
//   - the sentinel's cargo library builds the first-run prefetch the worker runs through;
//   - the host test target for the launch exists.
//
// These are source-shape checks (rank 3 of docs/standards/verification.md): they fail when a line is removed or moved and say nothing
// about whether a launch on a headset does it.

func permissionWaitBody(t *testing.T) string {
	t.Helper()
	return body(t, stripComments(read(t, "src/quest/sentinel/entry.cpp")), "bool InstallPermissionWaitHook(")
}

func TestTheWholeGameWorkerIsSetBeforeTheOptionsAreInstalled(t *testing.T) {
	t.Parallel()
	before(t, permissionWaitBody(t), "options.all_assets_ready", "nevr_quest::data_wiring::Install(options)")
}

func TestTheWholeGameWorkerIsTheProductionFirstRunWorker(t *testing.T) {
	t.Parallel()
	re := regexp.MustCompile(`options\.all_assets_ready\s*=\s*nevr_quest::cargo::MakeProductionFirstRunWorker\(\s*sentinel::ActiveConfig\(\)\.assets\s*\)`)
	if !re.MatchString(permissionWaitBody(t)) {
		t.Error("InstallPermissionWaitHook does not set options.all_assets_ready from MakeProductionFirstRunWorker(sentinel::ActiveConfig().assets)")
	}
}

func TestTheWholeGameModeAndTheWorkerComeFromTheSameConfiguration(t *testing.T) {
	t.Parallel()
	re := regexp.MustCompile(`options\.all_assets\s*=\s*sentinel::ActiveConfig\(\)\.assets\.wholeGame`)
	if !re.MatchString(permissionWaitBody(t)) {
		t.Error("InstallPermissionWaitHook does not set options.all_assets from sentinel::ActiveConfig().assets.wholeGame")
	}
}

func TestTheBackgroundFetchIsNotStartedInTheWholeGameMode(t *testing.T) {
	t.Parallel()
	code := stripComments(read(t, "src/quest/integration/production_steps.cpp"))
	if !regexp.MustCompile(`if\s*\(\s*ok\s*&&\s*!assets\.wholeGame\s*\)\s*nevr_quest::cargo::Start\(`).MatchString(code) {
		t.Error("production_steps.cpp starts the background cargo fetch without the !assets.wholeGame guard")
	}
}

func TestNoKnownWrongMarkerFor2398Remains(t *testing.T) {
	t.Parallel()
	for _, p := range []string{"src/quest/sentinel/entry.cpp", "docs/adr/0045-cdn-texture-delivery.md"} {
		if strings.Contains(read(t, p), "KNOWN-WRONG #2398") {
			t.Errorf("%s still carries KNOWN-WRONG #2398", p)
		}
	}
}

func TestTheSentinelBuildsThePrefetchTheWorkerRunsThrough(t *testing.T) {
	t.Parallel()
	cmake := read(t, "src/quest/sentinel/CMakeLists.txt")
	if !regexp.MustCompile(`add_library\(nevr_quest_cargo STATIC[^)]*core/cargo_prefetch\.cpp`).MatchString(cmake) {
		t.Error("nevr_quest_cargo does not build core/cargo_prefetch.cpp")
	}
}

func TestTheWholeGameLaunchHasAHostTestTarget(t *testing.T) {
	t.Parallel()
	if !strings.Contains(read(t, "src/quest/host_tests/groups/data.cmake"), "nevr_ht_test(whole_game_launch_test") {
		t.Error("src/quest/host_tests/groups/data.cmake has no whole_game_launch_test target")
	}
}
