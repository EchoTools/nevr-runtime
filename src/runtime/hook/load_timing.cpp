#include "runtime/hook/load_timing.h"

#include <winsock2.h>
#include <windows.h>

#include <chrono>
#include <cstdint>

#include "abi/echovr_functions.h"
#include "core/logging.h"
#include "nevr/pc/nevr_common.h"
#include "runtime/hook/addresses.h"
#include "runtime/hook/patching.h"
#include "runtime/hook/symbol_corpus.h"

namespace nevr_load_timing {
namespace {

constexpr std::uint64_t kFiletimeTicksPerMs = 10000;
constexpr std::uint64_t kFiletimeToUnixEpochTicks = 116444736000000000ULL;  // 1601-01-01 to 1970-01-01, 100 ns ticks

using LoadLevelFn = std::uint64_t(__fastcall*)(void* gameSpace, std::uint64_t p2, void* p3);
LoadLevelFn g_originalLoadLevel = nullptr;

nevr_load_timing::BootTimer g_boot;
nevr_load_timing::FirstLoad g_firstLoad;

std::uint64_t FiletimeTicks(const FILETIME& ft) {
  return (static_cast<std::uint64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
}

std::uint64_t NowTicks() {
  FILETIME now{};
  GetSystemTimeAsFileTime(&now);
  return FiletimeTicks(now);
}

std::uint64_t TicksToEpochMs(std::uint64_t ticks) {
  return ticks > kFiletimeToUnixEpochTicks ? (ticks - kFiletimeToUnixEpochTicks) / kFiletimeTicksPerMs : 0;
}

/// Milliseconds from this process's creation to now; kUnknownMsValue (-1) when the creation time cannot be read, so the
/// record says "never measured" and never a 0 that reads as a measurement.
std::int64_t ElapsedSinceProcessStartMs() {
  FILETIME create{}, exit{}, kernel{}, user{};
  if (!GetProcessTimes(GetCurrentProcess(), &create, &exit, &kernel, &user)) return nevr_load_timing::kUnknownMsValue;
  return nevr_load_timing::ElapsedMs(TicksToEpochMs(NowTicks()), TicksToEpochMs(FiletimeTicks(create)));
}

struct Clock {
  // steady_clock is QueryPerformanceCounter on this toolchain: GetTickCount64 steps in 10 to 16 ms, which is 16% of a 97 ms load.
  std::uint64_t MonotonicMs() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
  }
  std::uint64_t EpochMs() { return TicksToEpochMs(NowTicks()); }
};

constexpr char kHookName[] = "LoadLevel";

/// The Warning a hook that did not arm owes (logging.md, Rule 5): address, expected prologue and the bytes found. No
/// detour is installed, so level loads are not timed.
void LogNotArmed(const char* reason, const void* address) {
  Log(EchoVR::LogLevel::Warning, "%s",
      nevr_load_timing::FormatHookNotArmed(kHookName, reason,
                                           nevr::ECHOVR_DEFAULT_IMAGE_BASE + nevr_patch_addresses::CNCAGAMESPACE_LOAD_LEVEL,
                                           nevr_load_timing::kLoadLevelPrologue.data(), static_cast<const std::uint8_t*>(address),
                                           nevr_load_timing::kLoadLevelPrologue.size())
          .c_str());
}

void LogRecord(const std::string& line) { Log(EchoVR::LogLevel::Info, "%s", line.c_str()); }

std::uint64_t __fastcall LoadLevelDetour(void* gameSpace, std::uint64_t p2, void* p3) {
  return TimedLoad(p2, g_originalLoadLevel, gameSpace, p2, p3);
}

}  // namespace

std::uint64_t TimedLoad(std::uint64_t levelHash, std::uint64_t (*call)(void*, std::uint64_t, void*), void* gameSpace,
                        std::uint64_t p2, void* p3) {
  Clock clock;
  return nevr_load_timing::TimeLevelLoad(clock, LogRecord, g_firstLoad, levelHash, EchoVR::LookupSymbolName(levelHash),
                                         [&] { return call(gameSpace, p2, p3); });
}

void Install(bool joinHoldHooksIt) {
  if (joinHoldHooksIt) {
    Log(EchoVR::LogLevel::Info,
        "[NEVR.BOOT] load timing hook not installed reason=join_hold_hooks_load_level (the join hold's detour on "
        "LoadLevel times each load instead, so level_load records are still written)");
    return;
  }
  void* address = reinterpret_cast<void*>(reinterpret_cast<std::uintptr_t>(EchoVR::g_GameBaseAddress) +
                                          nevr_patch_addresses::CNCAGAMESPACE_LOAD_LEVEL);
  if (!nevr::ValidatePrologue(address, nevr_load_timing::kLoadLevelPrologue.data(), nevr_load_timing::kLoadLevelPrologue.size())) {
    LogNotArmed("prologue_mismatch", address);
    return;
  }
  g_originalLoadLevel = reinterpret_cast<LoadLevelFn>(address);
  if (!PatchDetour(&g_originalLoadLevel, reinterpret_cast<PVOID>(&LoadLevelDetour), kHookName)) {
    g_originalLoadLevel = nullptr;
    LogNotArmed("detour_failed", address);
    return;
  }
  Log(EchoVR::LogLevel::Info, "[NEVR.BOOT] load timing armed hook=LoadLevel (each level load now writes a level_load record)");
}

void OnGameLine(const char* line) {
  if (nevr_load_timing::IsEngineReadyLine(line)) g_boot.MarkEngineReady(ElapsedSinceProcessStartMs());
}

void OnLoggedIn() {
  std::string line;
  if (g_boot.MarkLoggedIn(ElapsedSinceProcessStartMs(), &line)) LogRecord(line);
}

}  // namespace nevr_load_timing
