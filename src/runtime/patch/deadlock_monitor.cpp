#include <windows.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

#include "abi/echovr_functions.h"
#include "core/logging.h"
#include "nevr/pc/nevr_common.h"
#include "runtime/hook/hook_liveness.h"
#include "runtime/hook/patching.h"
#include "runtime/patch/deadlock_monitor_loop.h"
#include "runtime/patch/mode_patches.h"

namespace {

// CDeadlockMonitor::Main(this), the thread procedure (vtable slot 2 at 0x1416E73B8). Its first 19 bytes.
constexpr std::uint64_t kThreadProcVA = 0x1401D3850;
constexpr std::array<std::uint8_t, 19> kThreadProcPrologue = {0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x83, 0x79, 0x18, 0x00,
                                                              0x48, 0x8B, 0xD9, 0x0F, 0x84, 0x7D, 0x00, 0x00, 0x00};
constexpr std::uint64_t kThreadProcVtableSlotVA = 0x1416E73B8;

// CTimer::Seconds(): returns the whole seconds as a 64-bit value; the monitor stores its low 32 bits.
constexpr std::uint64_t kTimerSecondsVA = 0x1400D05B0;
constexpr std::array<std::uint8_t, 11> kTimerSecondsPrologue = {0x48, 0x83, 0xEC, 0x28, 0x83, 0x3D,
                                                                0x7D, 0x8A, 0xFC, 0x01, 0x00};

// The monitor's fields, from the object pointer the thread procedure receives: running (int32) at +0x18, the last
// heartbeat (uint32) at +0x2C.
constexpr std::size_t kRunningOffset = 0x18;
constexpr std::size_t kLastBeatOffset = 0x2C;

using ThreadProcFn = void (*)(void* monitor);
using TimerSecondsFn = std::uint64_t (*)();

ThreadProcFn g_originalThreadProc = nullptr;
TimerSecondsFn g_timerSeconds = nullptr;
std::atomic<bool> g_firstRunLogged{false};

std::string HexBytes(const std::uint8_t* bytes, std::size_t count) {
  std::string out;
  char pair[4] = {};
  for (std::size_t i = 0; i < count; ++i) {
    std::snprintf(pair, sizeof(pair), "%02X", static_cast<unsigned int>(bytes[i]));
    out += pair;
  }
  return out;
}

// The thread procedure. Same loop as the game's, with the stall check removed: the heartbeat is refreshed every second
// and nothing is reported or stopped. The thread keeps its Sleep(1000) cadence until the game clears running.
void DeadlockMonitorMainHook(void* monitor) {
  nevr_hook_liveness::Mark(nevr_hook_liveness::kDeadlockMonitorMain);
  if (!g_firstRunLogged.exchange(true)) {
    Log(EchoVR::LogLevel::Info,
        "[NEVR.PATCH] deadlock monitor running panic_check=off reason=stock_stall_check_writes_to_null "
        "(a stall on a dedicated server would crash the process; the heartbeat is kept, nothing is reported)");
  }
  auto* base = static_cast<std::uint8_t*>(monitor);
  nevr_deadlock_monitor::RunLoop(
      reinterpret_cast<const std::int32_t*>(base + kRunningOffset),
      reinterpret_cast<std::uint32_t*>(base + kLastBeatOffset), [] { ::Sleep(1000); },
      [] { return g_timerSeconds(); });
}

}  // namespace

/// Replaces the game's CDeadlockMonitor::Main with a whole-function hook; the only part of the stock body not kept is its
/// stall check. Installed in every mode, as the byte patch was. Validates the thread procedure, its vtable slot and
/// CTimer::Seconds before it attaches; a mismatch leaves the stock thread in place and logs what was found.
VOID PatchDeadlockMonitor() {
  const auto base = reinterpret_cast<std::uintptr_t>(EchoVR::g_GameBaseAddress);

  void* timer = nevr::ResolveVA_Checked(base, kTimerSecondsVA);
  if (!nevr::ValidatePrologue(timer, kTimerSecondsPrologue.data(), kTimerSecondsPrologue.size())) {
    Log(EchoVR::LogLevel::Warning,
        "[NEVR.PATCH] deadlock monitor hook skipped reason=timer_prologue_mismatch va=0x%llX expected=%s actual=%s",
        static_cast<unsigned long long>(kTimerSecondsVA),
        HexBytes(kTimerSecondsPrologue.data(), kTimerSecondsPrologue.size()).c_str(),
        timer ? HexBytes(static_cast<const std::uint8_t*>(timer), kTimerSecondsPrologue.size()).c_str() : "unresolved");
    return;
  }

  void* proc = nevr::ResolveVA_Checked(base, kThreadProcVA);
  if (!nevr::ValidatePrologue(proc, kThreadProcPrologue.data(), kThreadProcPrologue.size())) {
    Log(EchoVR::LogLevel::Warning,
        "[NEVR.PATCH] deadlock monitor hook skipped reason=prologue_mismatch va=0x%llX expected=%s actual=%s",
        static_cast<unsigned long long>(kThreadProcVA),
        HexBytes(kThreadProcPrologue.data(), kThreadProcPrologue.size()).c_str(),
        proc ? HexBytes(static_cast<const std::uint8_t*>(proc), kThreadProcPrologue.size()).c_str() : "unresolved");
    return;
  }

  void* slotAddress = nevr::ResolveVA_Checked(base, kThreadProcVtableSlotVA);
  std::uint64_t slot = 0;
  if (slotAddress != nullptr) std::memcpy(&slot, slotAddress, sizeof(slot));
  if (slot != kThreadProcVA) {
    Log(EchoVR::LogLevel::Warning,
        "[NEVR.PATCH] deadlock monitor hook skipped reason=vtable_slot_mismatch va=0x%llX expected=0x%llX actual=0x%llX",
        static_cast<unsigned long long>(kThreadProcVtableSlotVA), static_cast<unsigned long long>(kThreadProcVA),
        static_cast<unsigned long long>(slot));
    return;
  }

  g_timerSeconds = reinterpret_cast<TimerSecondsFn>(timer);
  g_originalThreadProc = reinterpret_cast<ThreadProcFn>(proc);
  if (!PatchDetour(&g_originalThreadProc, reinterpret_cast<PVOID>(&DeadlockMonitorMainHook),
                   "CDeadlockMonitor::Main")) {
    return;  // PatchDetour has logged the MinHook reason at Warning (N126)
  }
  Log(EchoVR::LogLevel::Info,
      "[NEVR.PATCH] hook installed name=CDeadlockMonitor::Main thread_proc=0x%llX panic_check=off",
      static_cast<unsigned long long>(kThreadProcVA));
}
