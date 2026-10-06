// Compatible nevr.dll fixture for the crash harness (docs/design step 7): it
// links the REAL runtime crash instrumentation — lifecycle/crash_recovery.cpp
// (BreakpointVEH / WriteCrashDump / VehPrintf) and lifecycle/crash_dump_format.cpp
// — and installs the VEH from its initialize() entry, so a deliberate fault after
// a successful load produces the runtime's own [NEVR.CRASH] evidence, not just the
// host's BugSplat_createReport minidump.
//
// The runtime's crash code is deeply game-coupled (it patches game addresses via
// PatchDetour), so this fixture stubs only the game-facing seams the file reaches
// and never calls the game-hooking installers. InstallVEH() needs none of them:
// it caches the module table, reserves handler stack, and registers BreakpointVEH
// with AddVectoredExceptionHandler. The same production code IS the code under
// test (the test_behavioral N68 pattern), and it pins the split's module-attribution
// fix: BreakpointVEH must now recognise "nevr.dll" alongside "BugSplat64.dll".

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <windows.h>

#include "abi/echovr_functions.h"   // EchoVR::g_GameBaseAddress + LogLevel
#include "core/logging.h"           // Log, SetFatalErrorHandler, GetRunId, FatalErrorHandlerFunc
#include "core/runtime_abi.h"
#include "runtime/lifecycle/crash_recovery.h"
#include "runtime/hook/hook_guard.h"         // HookGuard::Record
#include "runtime/patch/binary_bug_fixes.h"  // BinaryBugFixes::Shutdown
#include "runtime/compat/ws_bridge.h"        // StopWebSocketBridgeListener

#include <cstdint>

// ---------------------------------------------------------------------------
// Stubs for game-facing symbols crash_recovery.cpp references but the fixture
// never exercises (Log, FatalError transport, the detour machinery's HookGuard
// side table, and the shutdown paths' in-process ws_bridge / BinaryBugFixes).
// These are declarations with C++ linkage exactly as the production headers
// spell them; the game-hooking installers are never invoked.
// ---------------------------------------------------------------------------

void Log(EchoVR::LogLevel, const char*, ...) {}

namespace HookGuard {
void Record(const void*, const char*) {}
}  // namespace HookGuard

namespace BinaryBugFixes {
void Shutdown() {}
}  // namespace BinaryBugFixes

void StopWebSocketBridgeListener() {}

void SetFatalErrorHandler(FatalErrorHandlerFunc) {}

const CHAR* GetRunId() {
  static CHAR id[] = "host-crash-runtime";
  return id;
}

// cli.h declares `extern BOOL g_isServer`; define it here (the fixture links no
// cli.cpp). Set to TRUE by Initialize so InstallVEH installs the VEH instead of
// early-returning on the Wine-client path.
BOOL g_isServer = FALSE;

namespace EchoVR {
CHAR* g_GameBaseAddress = nullptr;
}

namespace {

std::int32_t Initialize(const NevrHostContext* host) {
  if (host == nullptr) return 1;
  // Server mode arms the VEH: InstallVEH skips Wine client processes
  // (crash_recovery.cpp:948) and BreakpointVEH only reports null-deref/AV faults
  // when g_isServer is set.
  g_isServer = TRUE;
  InstallVEH();
  return 0;
}

void Before(void*) {}
void After(void*, std::uint64_t) {}

const NevrRuntimeApi kApi = {
    {sizeof(NevrRuntimeApi), NevrAbi::kMajor, NevrAbi::kMinor, 0},
    &Initialize,
    &Before,
    &After,
};

// The fault must land inside nevr.dll so BreakpointVEH attributes it to the
// runtime (inNevrModule). A fault in the test executable would be ignored.
__declspec(noinline) void FaultWrite(std::uintptr_t address) {
  *reinterpret_cast<volatile int*>(address) = 0xDEAD;
}

}  // namespace

extern "C" __declspec(dllexport) const NevrRuntimeApi* NEVR_GetRuntimeApi() {
  return &kApi;
}

extern "C" __declspec(dllexport) void NevrHostTest_CrashRuntimeFault() {
  FaultWrite(0);
}

BOOL WINAPI DllMain(HINSTANCE, DWORD, LPVOID) { return TRUE; }
