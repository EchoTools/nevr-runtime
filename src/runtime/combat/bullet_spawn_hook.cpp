#include "runtime/combat/bullet_spawn_hook.h"

#include <winsock2.h>
#include <windows.h>

#include <atomic>
#include <cstdio>
#include <cstring>

#include "core/logging.h"
#include "runtime/combat/write_gate.h"
#include "runtime/hook/patching.h"

namespace nevr_combat::bullet_spawn {
namespace {

using SpawnFn = void(__fastcall*)(std::uint8_t*, std::uint64_t, void*, float, const void*, const float*, int,
                                  std::uint32_t);

constexpr const char* kHookName = "CR15NetBulletCS::FireBullet";  // HookGuard's name for the one detour

SRWLOCK g_lock = SRWLOCK_INIT;           // guards g_target, g_real and the install / removal
void* g_target = nullptr;                // the hooked address while the detour is in place
// The trampoline while the detour is in place, else the game's function. PatchDetour publishes the trampoline here before it
// enables the detour (core/hook_lifecycle.h), so the first call through the detour already finds it.
SpawnFn g_real = nullptr;
std::atomic<DropFilter> g_drop{nullptr};
std::atomic<AfterFilter> g_after{nullptr};

void __fastcall Hooked(std::uint8_t* sys, std::uint64_t index, void* ref, float speed, const void* pose,
                       const float* velocity, int remote, std::uint32_t flags) {
  const Call call{sys, index, ref, speed, pose, velocity, remote, flags};
  if (const DropFilter drop = g_drop.load(std::memory_order_acquire); drop != nullptr && drop(call)) return;
  g_real(sys, index, ref, speed, pose, velocity, remote, flags);
  if (const AfterFilter after = g_after.load(std::memory_order_acquire)) after(call);
}

void Hex(const std::uint8_t* bytes, std::size_t n, char* out, std::size_t size) {
  std::size_t at = 0;
  for (std::size_t i = 0; i < n && at + 3 <= size; ++i) {
    at += static_cast<std::size_t>(std::snprintf(out + at, size - at, "%02x", bytes[i]));
  }
  if (at < size) out[at] = '\0';
}

// Called with g_lock held. True when the detour is in place afterwards.
bool InstallLocked(std::uintptr_t gameBase, const char* module) {
  if (g_target != nullptr) return true;
  auto* const target = reinterpret_cast<std::uint8_t*>(gameBase + kRva);
  if (std::memcmp(target, kPrologue, sizeof(kPrologue)) != 0) {
    char expected[2 * sizeof(kPrologue) + 1];
    char actual[2 * sizeof(kPrologue) + 1];
    Hex(kPrologue, sizeof(kPrologue), expected, sizeof(expected));
    Hex(target, sizeof(kPrologue), actual, sizeof(actual));
    Log(EchoVR::LogLevel::Warning,
        "[NEVR.COMBAT] bullet spawn hook not installed module=%s va=%p rva=0x%llX reason=prologue_mismatch expected=%s "
        "actual=%s (not the supported echovr.exe build, or something else wrote there; the module's bullet rule stays off)",
        module, static_cast<void*>(target), static_cast<unsigned long long>(kRva), expected, actual);
    return false;
  }
  // GateWrite logs the overlap itself (the runtime range it met); the detour writes up to the 16 bytes HookGuard compares.
  if (!GateWrite(target, 16, kHookName)) {
    Log(EchoVR::LogLevel::Warning,
        "[NEVR.COMBAT] bullet spawn hook not installed module=%s va=%p rva=0x%llX reason=overlap (the module's bullet rule "
        "stays off)",
        module, static_cast<void*>(target), static_cast<unsigned long long>(kRva));
    return false;
  }
  g_real = reinterpret_cast<SpawnFn>(target);
  if (!PatchDetour(&g_real, reinterpret_cast<PVOID>(&Hooked), kHookName)) {  // PatchDetour logs the failed stage (N126)
    Log(EchoVR::LogLevel::Warning,
        "[NEVR.COMBAT] bullet spawn hook not installed module=%s va=%p rva=0x%llX reason=%s (the module's bullet rule "
        "stays off)",
        module, static_cast<void*>(target), static_cast<unsigned long long>(kRva), Hooking::LastAttachError());
    return false;
  }
  g_target = target;
  Log(EchoVR::LogLevel::Info,
      "[NEVR.COMBAT] bullet spawn hook applied module=%s va=%p rva=0x%llX (one detour; PowerBoost's drop and DroneFix's "
      "tracer rule are filters on it, in that order)",
      module, static_cast<void*>(target), static_cast<unsigned long long>(kRva));
  return true;
}

// Called with g_lock held, after a filter was cleared: the detour goes when no filter is left.
void RemoveIfUnusedLocked() {
  if (g_target == nullptr || g_drop.load(std::memory_order_acquire) != nullptr ||
      g_after.load(std::memory_order_acquire) != nullptr) {
    return;
  }
  const MH_STATUS status = PatchRetire(g_target);
  if (status != MH_OK && status != MH_ERROR_NOT_CREATED) {
    Log(EchoVR::LogLevel::Warning,
        "[NEVR.COMBAT] bullet spawn hook removal failed va=%p status=%s (the detour stays in place, calling no filter)",
        g_target, MH_StatusToString(status));
    return;
  }
  Log(EchoVR::LogLevel::Info, "[NEVR.COMBAT] bullet spawn hook removed va=%p (no filter left on it)", g_target);
  // The game's bytes are back: a call already past the filters reaches the game's own function, not a freed trampoline.
  g_real = reinterpret_cast<SpawnFn>(g_target);
  g_target = nullptr;
}

template <typename F>
bool Set(std::atomic<F>& slot, std::uintptr_t gameBase, F filter, const char* module, const char* which) {
  const char* const name = module != nullptr ? module : "(unnamed)";
  AcquireSRWLockExclusive(&g_lock);
  if (filter == nullptr) {
    const bool had = slot.exchange(nullptr, std::memory_order_acq_rel) != nullptr;
    if (had) Log(EchoVR::LogLevel::Info, "[NEVR.COMBAT] bullet spawn filter cleared filter=%s module=%s", which, name);
    RemoveIfUnusedLocked();
    ReleaseSRWLockExclusive(&g_lock);
    return true;
  }
  const bool installed = InstallLocked(gameBase, name);
  if (installed) {
    slot.store(filter, std::memory_order_release);
    Log(EchoVR::LogLevel::Info, "[NEVR.COMBAT] bullet spawn filter set filter=%s module=%s", which, name);
  }
  ReleaseSRWLockExclusive(&g_lock);
  return installed;
}

}  // namespace

bool SetDrop(std::uintptr_t gameBase, DropFilter filter, const char* module) {
  return Set(g_drop, gameBase, filter, module, "drop");
}

bool SetAfter(std::uintptr_t gameBase, AfterFilter filter, const char* module) {
  return Set(g_after, gameBase, filter, module, "after");
}

bool Installed() {
  AcquireSRWLockShared(&g_lock);
  const bool on = g_target != nullptr;
  ReleaseSRWLockShared(&g_lock);
  return on;
}

}  // namespace nevr_combat::bullet_spawn
