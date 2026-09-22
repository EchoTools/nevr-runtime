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

#include <jni.h>
#include <android/log.h>
#include <time.h>

#include <atomic>
#include <cstdint>

#define NEVR_TAG "NEVR-Sentinel"

namespace {

// "The basics" GOT hook (Andrew, 2026-09-15): prove we can intercept a real
// call libr15.so makes on live hardware, without needing libr15.so's own
// internal functions reconstructed in ReVault first (that reconstruction is
// separate, ongoing work — see docs/design/2026-09-15-*). clock_gettime is
// chosen deliberately: its signature is unambiguous POSIX (no risk of a
// wrong-arity/wrong-return-type call corrupting the engine's real args), and
// it's called continuously by any real-time engine loop, so a live counter
// climbing in logcat while sitting in a lobby is an immediate, unambiguous
// "did the hook take" signal — no need to wait for a specific game event.
using ClockGettimeFn = int (*)(clockid_t, struct timespec*);
ClockGettimeFn        g_origClockGettime = nullptr;
std::atomic<uint64_t> g_clockGettimeCalls{0};

int HookedClockGettime(clockid_t clk_id, struct timespec* tp) {
    const uint64_t n = g_clockGettimeCalls.fetch_add(1, std::memory_order_relaxed) + 1;
    // Every 300th call: several lines/sec at a real engine's tick rate without
    // flooding logcat. Real work would filter/aggregate; this is a proof.
    if (n % 300 == 1) {
        __android_log_print(ANDROID_LOG_INFO, NEVR_TAG,
                            "hook: libr15.so clock_gettime call #%llu (GOT hook live)",
                            static_cast<unsigned long long>(n));
    }
    return g_origClockGettime(clk_id, tp);
}

void InstallBasicsHook() {
    void* original = nullptr;
    const bool ok = sentinel::HookImport("libr15.so", "clock_gettime",
                                         reinterpret_cast<void*>(HookedClockGettime), &original);
    if (ok) {
        g_origClockGettime = reinterpret_cast<ClockGettimeFn>(original);
        __android_log_print(ANDROID_LOG_INFO, NEVR_TAG,
                            "hook: GOT-hooked libr15.so's clock_gettime import (basics proof)");
    } else {
        // Lookup miss or module not yet mapped — log and continue. A failed
        // hook install must never be fatal to the host process.
        __android_log_print(ANDROID_LOG_WARN, NEVR_TAG,
                            "hook: could not GOT-hook libr15.so clock_gettime "
                            "(lookup miss or module not yet loaded)");
    }
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
