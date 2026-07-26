/* nevr-quest crash-reporter sentinel.
 *
 * The Android/arm64 analogue of nevr-sentinel-dll (Windows). Ships under the
 * name libovrplatformloader.so, hijacking the APK-bundled Oculus Platform
 * loader so a load-time constructor arms breakpad before libr15's game code
 * runs, then forwards to the renamed original (libovrplatformloader_orig.so).
 *
 * See docs/design/2026-07-13-quest-crash-reporter-injection.md.
 */
#pragma once

#include <string>

namespace sentinel {

// Where crash artifacts (minidump + maps + backtrace) are written. Resolved
// from a compile-time default and the first app-writable candidate directory.
// Never from an environment variable (repo rule).
struct Config {
    std::string dumpDir;
};

// Install the breakpad ExceptionHandler for SIGSEGV/SIGABRT/SIGBUS/SIGILL/
// SIGFPE. Idempotent and thread-safe; safe to call from both the ELF
// constructor and JNI_OnLoad.
void Arm();

// Tear down the handler (best-effort; mainly for tests/teardown).
void Disarm();

// The resolved output directory (valid after Arm()).
const char* DumpDir();

}  // namespace sentinel
