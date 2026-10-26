// frame_stats.cpp — the runtime side of the #954 frame-time probe. The pure parts (rings, window math, lines)
// are in frame_stats.h; this file owns the one hook, the switch's state and the log call.

#include "runtime/frame/frame_stats.h"

#include <windows.h>

#include <atomic>

#include "core/build_identity.h"
#include "core/logging.h"
#include "nevr_common.h"
#include "runtime/hook/hook_liveness.h"
#include "runtime/hook/patching.h"

namespace nevr_frame_stats {
namespace {

// ovr_GetPerfStats wrapper at echovr.exe 0x1413662e0: MOV RAX,[RIP+0xd854a1]. The first instruction is the
// seven bytes MinHook relocates into its trampoline (it needs five to place the jump).
constexpr uint8_t kPerfStatsPrologue[7] = {0x48, 0x8B, 0x05, 0xA1, 0x54, 0xD8, 0x00};

using PerfStatsFn = int32_t(__fastcall*)(void* session, void* outStats);
PerfStatsFn g_originalPerfStats = nullptr;

// Written once by Configure (boot, one thread) before the hooks that read them can matter, read by the hooks.
std::atomic<Mode> g_mode{Mode::Off};
std::atomic<bool> g_frameRingOn{false};
std::atomic<bool> g_perfOn{false};

SpscRing<FrameSlot, kFrameRingSlots> g_frameRing;
SpscRing<PerfSample, kPerfRingSlots> g_perfRing;
std::atomic<uint64_t> g_perfRejected{0};

// Poll's state. The dispatcher's re-entrancy gate (tick.cpp, g_tickReentry) makes Poll single-threaded, and the
// gate's interlocked release publishes it to whichever thread polls next.
Collector g_collector;
DrainState g_drain;
WindowClock g_clock;

// Runs on the game's thread inside ovr_GetPerfStats. Calls the original once, never a second time (a second
// call would race the session and split FrameStats), copies only on success, and logs nothing.
int32_t __fastcall PerfStatsHook(void* session, void* outStats) {
  nevr_hook_liveness::Mark(nevr_hook_liveness::kOvrGetPerfStats);
  const int32_t result = g_originalPerfStats(session, outStats);
  if (ShouldCopyPerf(result, outStats, g_perfOn.load(std::memory_order_relaxed))) {  // OVR_SUCCESS(result)
    CapturePerf(outStats, g_perfRing, g_perfRejected);
  }
  return result;
}

const char* InstallPerfStatsHook(uintptr_t gameBase, bool isServer) {
  if (isServer) return "skipped_server";  // a dedicated server has no VR session
  const void* site = nevr::ResolveVA_Checked(gameBase, nevr::ECHOVR_DEFAULT_IMAGE_BASE +
                                                           nevr_patch_addresses::OVR_GET_PERF_STATS);
  if (!nevr::ValidatePrologue(site, kPerfStatsPrologue, sizeof(kPerfStatsPrologue))) {
    Log(EchoVR::LogLevel::Warning, "[NEVR.PERF] ovr_GetPerfStats hook skipped va=0x%llx reason=prologue_mismatch",
        static_cast<unsigned long long>(nevr::ECHOVR_DEFAULT_IMAGE_BASE + nevr_patch_addresses::OVR_GET_PERF_STATS));
    return "prologue_mismatch";
  }
  g_originalPerfStats = reinterpret_cast<PerfStatsFn>(gameBase + nevr_patch_addresses::OVR_GET_PERF_STATS);
  if (!PatchDetour(&g_originalPerfStats, reinterpret_cast<PVOID>(&PerfStatsHook), "ovr_GetPerfStats")) {
    g_originalPerfStats = nullptr;
    return "hook_failed";  // PatchDetour has logged the cause at Warning (N126)
  }
  return "installed";
}

}  // namespace

void Configure(uintptr_t gameBase, bool isServer, Mode mode, bool recognised) {
  bool callSiteMatches = false;
  if (mode == Mode::Full) {
    const void* site = nevr::ResolveVA_Checked(gameBase, nevr::ECHOVR_DEFAULT_IMAGE_BASE + kLoopHeadCallRva);
    callSiteMatches = LoopHeadCallMatches(site);
    if (!callSiteMatches) {
      Log(EchoVR::LogLevel::Warning,
          "[NEVR.PERF] frame ring not armed va=0x%llx reason=loop_head_call_mismatch",
          static_cast<unsigned long long>(nevr::ECHOVR_DEFAULT_IMAGE_BASE + kLoopHeadCallRva));
    }
  }
  const char* frameRing = FrameRingState(mode, callSiteMatches);
  g_frameRingOn.store(ArmsFrameRing(mode, callSiteMatches), std::memory_order_release);
  // The copy is installed on every client whatever the switch says (so the ring's own cost can be measured
  // against the game's app_cpu with the ring off); it keeps what it reads only while the probe is on.
  const char* perfHook = InstallPerfStatsHook(gameBase, isServer);
  g_perfOn.store(KeepsPerfStats(mode), std::memory_order_release);
  g_mode.store(mode, std::memory_order_release);

  if (!recognised) {
    Log(EchoVR::LogLevel::Warning,
        "[NEVR.PERF] diagnostics.frame_stats has a value this build does not know (true, false or perf); using off");
  }
  char line[512];
  FormatStartLine(line, sizeof(line), mode, recognised, isServer ? "server" : "client",
                  nevr_build_identity::Get().git_describe.c_str(), frameRing, perfHook);
  Log(EchoVR::LogLevel::Info, "%s", line);
}

void OnLoopHead(uint64_t nowUs) {
  if (!ShouldRecordLoopHead(nowUs, g_frameRingOn.load(std::memory_order_relaxed))) return;
  RecordLoopHead(g_frameRing, nowUs);
}

void Poll(uint64_t nowUs) {
  if (g_mode.load(std::memory_order_acquire) == Mode::Off) return;

  Drain(g_frameRing, g_perfRing, g_perfRejected, g_drain, g_collector);
  uint64_t elapsedMs = 0;
  if (!g_clock.Tick(nowUs, &elapsedMs)) return;
  const WindowStats window = g_collector.Finish(elapsedMs);
  char line[512];
  FormatWindowLine(line, sizeof(line), window);
  Log(EchoVR::LogLevel::Info, "%s", line);
}

}  // namespace nevr_frame_stats
