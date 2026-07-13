/* nevr_quest toolchain probe.
 *
 * Trivial translation unit whose only purpose is to prove the NDK r26d
 * arm64-v8a build path compiles and links a shared object cleanly.
 * Not shipped; not part of the crash reporter.
 */
#include <android/log.h>

extern "C" {

// Marker export so the artifact is identifiable in `nm -D`.
__attribute__((visibility("default")))
const char* nevr_toolchain_probe_marker() {
    return "nevr-quest-toolchain-probe";
}

// Touch liblog so the -llog link path is exercised.
__attribute__((visibility("default")))
void nevr_toolchain_probe_log() {
    __android_log_print(ANDROID_LOG_INFO, "NEVR-Probe", "toolchain ok");
}

}  // extern "C"
