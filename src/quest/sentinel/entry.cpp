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
#include "pinned_targets.h"

#include <jni.h>
#include <android/log.h>

#include <atomic>
#include <cstdint>

#define NEVR_TAG "NEVR-Sentinel"

namespace {

// "The basics" GOT hook: prove we can intercept a real call libr15.so makes on
// live hardware, without needing libr15.so's own internal functions
// reconstructed first. clock_gettime is chosen deliberately: its signature is
// unambiguous POSIX (no risk of a wrong-arity/wrong-return-type call corrupting
// the engine's real args), and it's called continuously by any real-time engine
// loop, so a live counter climbing in logcat while sitting in a lobby is an
// immediate, unambiguous "did the hook take" signal.
using ClockThunk = sentinel::pinned::ClockGettimeThunk;
sentinel::GotHook     g_clockHook;
std::atomic<uint64_t> g_clockGettimeCalls{0};

int HookedClockGettime(ClockThunk::Fn original, clockid_t clk_id, struct timespec* tp) {
    const uint64_t n = g_clockGettimeCalls.fetch_add(1, std::memory_order_relaxed) + 1;
    // Every 300th call: several lines/sec at a real engine's tick rate without
    // flooding logcat. Real work would filter/aggregate; this is a proof.
    if (n % 300 == 1) {
        __android_log_print(ANDROID_LOG_INFO, NEVR_TAG,
                            "hook: libr15.so clock_gettime call #%llu (GOT hook live)",
                            static_cast<unsigned long long>(n));
    }
    return original(clk_id, tp);
}

// A failed install is logged by GotHook with its status and leaves the original
// call intact; it is never fatal to the host process.
void InstallBasicsHook() {
    ClockThunk::Arm(&HookedClockGettime);
    g_clockHook.Install(sentinel::pinned::LibR15ClockGettime(), ClockThunk::EntryAddress(),
                        ClockThunk::OriginalOut());
}

}  // namespace

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
    __android_log_print(ANDROID_LOG_INFO, NEVR_TAG,
                        "constructor: arming crash reporter (pre-libr15)");
    sentinel::Arm();
    InstallBasicsHook();
}

// Belt-and-suspenders: if anything System.loadLibrary's this by name, arm here
// too. (Under DT_NEEDED loading the runtime does not auto-call this.)
JNIEXPORT jint JNI_OnLoad(JavaVM* /*vm*/, void* /*reserved*/) {
    sentinel::Arm();
    return JNI_VERSION_1_6;
}

}  // extern "C"
