package questwiring

import (
	"strings"
	"testing"
)

// ADR 0048: the PC's custom-map fetch worker is stopped, with its bounded wait, on both ways the process ends. The stop itself
// (nevr_map_channel::FetchWorker::Stop) is driven by the host tests; crash_recovery.cpp is in no test target, so the two calls are
// pinned here by source shape: a client's normal exit stops the worker before it returns from the game-loop wrapper, and every
// other shutdown stops it in PerformGracefulShutdown before ForceFatalExit kills the thread.

const mapFetchStop = "nevr_map_channel::pc::StopFetch();"

func TestAClientsNormalExitStopsTheMapFetchBeforeItReturns(t *testing.T) {
	b := body(t, stripComments(read(t, "src/runtime/lifecycle/crash_recovery.cpp")), "static INT64 GameMainWrapperHook(INT64 arg1) {")
	at := strings.Index(b, "if (!g_isServer) {")
	if at < 0 {
		t.Fatal("GameMainWrapperHook has no client branch (if (!g_isServer) {)")
	}
	// The client branch's own return is the first one after its guard; the server's exit comes after it.
	before(t, b[at:], mapFetchStop, "return gameResult;")
}

func TestGracefulShutdownStopsTheMapFetchBeforeTheForcedExit(t *testing.T) {
	b := body(t, stripComments(read(t, "src/runtime/lifecycle/crash_recovery.cpp")), "void PerformGracefulShutdown(unsigned int exitCode) {")
	before(t, b, mapFetchStop, "ForceFatalExit(exitCode);")
}

// The Quest's twin: the process-end callback stops the map worker after the plugin records and before the self-check report, with a
// wait the build checks is under the hook's before-exit budget (the stop waits for a transfer to see it; it aborts none); and the
// channel's start publishes its state for that stop only once the worker runs.
func TestTheQuestProcessEndStopsTheMapFetchWithinTheBudget(t *testing.T) {
	b := body(t, stripComments(read(t, "src/quest/integration/production_steps.cpp")), "void OnGameProcessEnd() noexcept {")
	const stop = "nevr_quest::maps::Stop(nevr_quest::maps::kProcessEndStopWait);"
	before(t, b, "FinishPlugins(", stop)
	before(t, b, "NoteCombatProcessEnd();", stop)
	before(t, b, stop, "ReportAndroidMainReturned();")
	if !strings.Contains(stripComments(read(t, "src/quest/cdn/map_service.cpp")),
		"static_assert(nevr_quest::maps::kProcessEndStopWait.count() < nevr_quest::process_end::kFlushBudgetMs,") {
		t.Error("map_service.cpp does not check kProcessEndStopWait against the process-end budget")
	}
}

func TestTheQuestStartPublishesItsStateAfterTheWorkerStarts(t *testing.T) {
	b := body(t, stripComments(read(t, "src/quest/cdn/map_service.cpp")), "bool Start(nevr_cdn::Env* env,")
	before(t, b, "state->worker->Start();", "g_state.store(state);")
}

// Both platforms' stops are the core's one StopChannel over the channel's own request entry, worker and wake pointer.
func TestThePcStopFetchIsTheCoreStopOverItsChannel(t *testing.T) {
	b := body(t, stripComments(read(t, "src/runtime/content/map_channel.cpp")), "void StopFetch() {")
	want := "nevr_map_channel::StopChannel(st->requests.get(), st->worker.get(), &st->worker_ptr, nevr_map_channel::kStopWait)"
	if !strings.Contains(b, want) {
		t.Errorf("StopFetch does not contain %q", want)
	}
	before(t, b, "g_running.load()", "StopChannel(")
}

// The PC's stop runs on a shutdown thread while the boot thread may still be starting the channel: it reads only the state published,
// through an atomic, after a worker started (on the client's path and on the server's).
func TestThePcPublishesItsRunningStateAfterTheWorkerStarts(t *testing.T) {
	b := body(t, stripComments(read(t, "src/runtime/content/map_channel.cpp")), "void StartFetchUnguarded() {")
	before(t, body(t, b, "if (st->server) {"), "StartServerWorker(st);", "g_running.store(st);")
	client := b[strings.Index(b, "st->worker = std::make_unique<FetchWorker>("):]
	before(t, client, "st->worker->Start();", "g_running.store(st);")
}

func TestTheQuestStopIsTheCoreStopOverItsChannel(t *testing.T) {
	b := body(t, stripComments(read(t, "src/quest/cdn/map_service.cpp")), "void Stop(std::chrono::milliseconds wait) noexcept {")
	want := "nevr_map_channel::StopChannel(state->requests.get(), state->worker.get(), &state->worker_ptr, wait)"
	if !strings.Contains(b, want) {
		t.Errorf("maps::Stop does not contain %q", want)
	}
	before(t, b, "g_state.load()", "StopChannel(")
}
