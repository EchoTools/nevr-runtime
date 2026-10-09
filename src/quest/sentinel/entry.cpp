/* nevr-quest crash-reporter — load-time entry glue.
 *
 * This library ships as libovrplatformloader.so. It is pulled into the process
 * as a DT_NEEDED dependency of libr15.so (and libpnsovr/libpnsrad), so its ELF
 * constructor runs before any game code — the arm64/Bionic analogue of the
 * Windows BugSplat64.dll static-import trick. The real Oculus Platform loader
 * is renamed libovrplatformloader_orig.so and pulled in via an added DT_NEEDED
 * (see the patchelf --add-needed post-build step); its 1420 exports resolve to
 * consumers through Bionic's local-group symbol lookup.
 */

#include "sentinel.h"
#include "got_hook.h"
#include "hook_install.h"
#include "hook_log.h"
#include "hook_report.h"
#include "login_prompt_hook.h"
#include "pinned_targets.h"
#include "quest/integration/entry_hooks.h"

#include <jni.h>

#include <atomic>
#include <cstdint>

namespace {

// "The basics" GOT hook: prove we can intercept a real call libr15.so makes on
// live hardware, without needing libr15.so's own internal functions
// reconstructed first. clock_gettime is chosen deliberately: its signature is
// unambiguous POSIX (no risk of a wrong-arity/wrong-return-type call corrupting
// the engine's real args), and it's called continuously by any real-time engine
// loop. The install line in the log says the slot was patched; the reporter thread
// (hook_report.h) logs the call counter when it first moves and, if it keeps moving, at
// most once a minute.
//
// This translation unit is built with -fno-exceptions (callback_thunk.h requires
// it): the handler is noexcept and nothing in it can unwind.
using ClockThunk = sentinel::pinned::ClockGettimeThunk;
sentinel::GotHook     g_clockHook;
std::atomic<uint64_t> g_clockGettimeCalls{0};

int HookedClockGettime(ClockThunk::Fn original, clockid_t clk_id, struct timespec* tp) noexcept {
    // Runs on every clock_gettime libr15 makes, on any thread, possibly from a signal
    // handler: one atomic increment and the original call, nothing else.
    g_clockGettimeCalls.fetch_add(1, std::memory_order_relaxed);
    return original(clk_id, tp);
}

// The hook: the thunk's entry and its handler, recorded for the build-time frame sensor.
NEVR_HOOK_RECORD(kClockHook, ClockThunk, &HookedClockGettime);

}  // namespace

namespace nevr_quest::integration {

// The counters are registered by the constructor sequence together with every other hook's, before
// the single StartReporter (ctor_sequence.h).
bool RegisterClockCounters() noexcept {
    bool ok = sentinel::RegisterReportCounter("clock_gettime_calls", &g_clockGettimeCalls);
    ok = sentinel::RegisterReportCounter("clock_gettime_thunk_faults", &ClockThunk::FaultCounter(),
                                         sentinel::ReportKind::kFaults) && ok;
    return ok;
}

// A failed install is logged by GotHook with its status and leaves the original
// call intact; it is never fatal to the host process.
bool InstallClockHook() noexcept {
    ClockThunk::Arm(kClockHook);
    return sentinel::InstallThunk<ClockThunk>(g_clockHook, sentinel::pinned::LibR15ClockGettime()) ==
           sentinel::GotStatus::kOk;
}

}  // namespace nevr_quest::integration

extern "C" {

// Ground-truth marker export: proves our code is in the artifact (BAC-2).
__attribute__((visibility("default")))
const char* nevr_sentinel_marker() {
    return "nevr-quest-sentinel v1 (libovrplatformloader hijack, breakpad)";
}

// Earliest reachable hook: runs while the dynamic linker resolves libr15's
// DT_NEEDED closure, before libr15's JNI_OnLoad / ANativeActivity_onCreate.
__attribute__((constructor))
static void nevr_sentinel_ctor() {
    sentinel::LogFields(sentinel::LogLevel::kInfo, "sentinel_ctor", {{"action", "begin"}});
    // Everything the constructor does is one sequence (integration/ctor_sequence.h): crash reporter,
    // configuration, counters, the single reporter start, then each hook gated by its feature switch.
    nevr_quest::integration::RunSentinelConstructor();
}

// Belt-and-suspenders: if anything System.loadLibrary's this by name, arm here
// too. (Under DT_NEEDED loading the runtime does not auto-call this.)
JNIEXPORT jint JNI_OnLoad(JavaVM* /*vm*/, void* /*reserved*/) {
    sentinel::Arm();
    return JNI_VERSION_1_6;
}

}  // extern "C"
