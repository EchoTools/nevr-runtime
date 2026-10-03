#include "runtime/patch/social_facade.h"

#include <windows.h>
#include <MinHook.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <mutex>

#include "abi/echovr_functions.h"
#include "core/logging.h"
#include "nevr_common.h"
#include "runtime/hook/patching.h"
#include "runtime/lifecycle/config.h"
#include "runtime/lifecycle/service_config.h"
#include "runtime/patch/party_invite_gate.h"

namespace SocialFacade {
namespace {

constexpr std::uint64_t kAccessorVA = 0x1406169C0;
constexpr std::uint64_t kProviderNameVA = 0x1400EB030;
constexpr std::uint64_t kSocialJsonVA = 0x14015FDB0;
constexpr std::uint64_t kJsonSetVA = 0x1405EEA70;
constexpr std::uint64_t kJsonNavigateForWriteVA = 0x1405F0180;
constexpr std::uintptr_t kNetGameOffset = 0x8518;
constexpr std::uintptr_t kSocialDisabledOffset = 0x647E9;
constexpr std::array<std::uint8_t, 18> kAccessorPrologue = {0x48, 0x89, 0x5C, 0x24, 0x18, 0x48, 0x89, 0x74, 0x24,
                                                            0x20, 0x57, 0x48, 0x81, 0xEC, 0x30, 0x02, 0x00, 0x00};
constexpr std::array<std::uint8_t, 18> kSocialJsonPrologue = {
    0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0xD9,
    0x48, 0x8B, 0x01, 0xFF, 0x90, 0xC0, 0x00, 0x00, 0x00};
constexpr std::array<std::uint8_t, 21> kJsonSetPrologue = {
    0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x6C, 0x24, 0x18, 0x56,
    0x57, 0x41, 0x56, 0x48, 0x81, 0xEC, 0x20, 0x04, 0x00, 0x00};
constexpr std::array<std::uint8_t, 19> kJsonNavigateForWritePrologue = {
    0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C, 0x24, 0x10,
    0x48, 0x89, 0x74, 0x24, 0x18, 0x57, 0x48, 0x83, 0xEC};
constexpr std::uintptr_t kFacadeJsonOffset = 0x1F0;
constexpr std::uintptr_t kFacadeFlagsOffset = 0x27C;
constexpr std::uint32_t kTraceInitialCalls = 5;
constexpr std::uint32_t kTraceInterval = 300;

using AccessorFn = void* (*)(void* provider);
using ProviderNameFn = void (*)(void* provider, char* name);
using SocialJsonFn = void* (*)(void* social);
using JsonSetFn = std::uint32_t (*)(void* json, const char* path, std::uint32_t required);
using JsonNavigateForWriteFn = void* (*)(const char* path, void* json, void* outA, void* outB);
AccessorFn g_originalAccessor = nullptr;
ProviderNameFn g_providerName = nullptr;
std::atomic<SocialJsonFn> g_originalSocialJson{nullptr};
std::atomic<JsonSetFn> g_originalJsonSet{nullptr};
std::atomic<JsonNavigateForWriteFn> g_originalJsonNavigateForWrite{nullptr};
std::atomic<void*> g_socialJsonTarget{nullptr};
std::atomic<void*> g_jsonSetTarget{nullptr};
std::atomic<void*> g_jsonNavigateForWriteTarget{nullptr};
std::atomic<void*> g_facadeObject{nullptr};
std::atomic<void*> g_facadeJson{nullptr};
std::uintptr_t g_gameBase = 0;
std::once_flag g_jsonHooksOnce;
std::once_flag g_inviteGateOnce;
std::atomic<std::uint32_t> g_accessorCalls{0};
std::atomic<std::uint32_t> g_socialJsonCalls{0};
std::atomic<std::uint32_t> g_jsonSetCalls{0};
std::atomic<std::uint32_t> g_jsonNavigateForWriteCalls{0};

std::uint32_t CountTrace(std::atomic<std::uint32_t>& counter) {
  return counter.fetch_add(1, std::memory_order_relaxed) + 1;
}

bool ShouldLog(std::uint32_t callCount) {
  return callCount <= kTraceInitialCalls || callCount % kTraceInterval == 0;
}

std::uintptr_t JsonWord(const void* json, std::uintptr_t offset) {
  std::uintptr_t value = 0;
  std::memcpy(&value, static_cast<const std::uint8_t*>(json) + offset, sizeof(value));
  return value;
}

bool Readable(const void* address, std::size_t length) {
  if (address == nullptr || length == 0) return false;
  MEMORY_BASIC_INFORMATION info{};
  if (VirtualQuery(address, &info, sizeof(info)) == 0 || info.State != MEM_COMMIT) return false;
  if ((info.Protect & (PAGE_NOACCESS | PAGE_GUARD)) != 0) return false;
  const auto begin = reinterpret_cast<std::uintptr_t>(address);
  const auto end = reinterpret_cast<std::uintptr_t>(info.BaseAddress) + info.RegionSize;
  return begin <= end && length <= end - begin;
}

int SocialDisabledByte() {
  if (!Readable(g_pGame, kNetGameOffset + sizeof(void*))) return -1;
  void* netGame = nullptr;
  std::memcpy(&netGame, static_cast<const std::uint8_t*>(g_pGame) + kNetGameOffset, sizeof(netGame));
  if (netGame == nullptr) return -1;
  const auto* disabled = static_cast<const std::uint8_t*>(netGame) + kSocialDisabledOffset;
  return Readable(disabled, 1) ? static_cast<int>(*disabled) : -1;
}

void EnsureJsonHooksInstalled(void* facadeObject);

void* AccessorHook(void* provider) {
  void* result = g_originalAccessor(provider);
  char providerName[512] = "<unresolved>";
  if (provider != nullptr && g_providerName != nullptr) g_providerName(provider, providerName);
  providerName[sizeof(providerName) - 1] = '\0';
  const bool enabled = NevrCfgSocialFacadeEnabled();
  void* returned = Select(enabled, result);
  if (enabled && returned != result) {
    EnsureJsonHooksInstalled(returned);
  }
  // Config is only readable once the CLI is parsed (service_config.cpp NevrCfg), which is why this
  // is installed from the first accessor call and not from boot.
  if (enabled) std::call_once(g_inviteGateOnce, [] { PartyInviteGate::Install(g_gameBase); });
  FlushJsonTraces();
  const std::uint32_t callCount = g_accessorCalls.fetch_add(1, std::memory_order_relaxed) + 1;
  Log(EchoVR::LogLevel::Info,
      "[NEVR.SOCIAL] accessor provider=%s provider_ptr=%p original=%p returned=%p "
      "disabled_byte=%d facade_enabled=%d call_count=%u",
      providerName, provider, result, returned, SocialDisabledByte(), enabled ? 1 : 0, callCount);
  return returned;
}

void* SocialJsonHook(void* social) {
  const SocialJsonFn original = g_originalSocialJson.load(std::memory_order_acquire);
  if (original == nullptr || original == &SocialJsonHook ||
      reinterpret_cast<void*>(original) == g_socialJsonTarget.load(std::memory_order_acquire)) {
    static std::uintptr_t emptyJson[2] = {};
    return emptyJson;
  }
  void* result = original(social);
  if (social != g_facadeObject.load(std::memory_order_acquire)) return result;
  const std::uint32_t callCount = CountTrace(g_socialJsonCalls);
  if (!ShouldLog(callCount)) return result;
  void* json = g_facadeJson.load(std::memory_order_acquire);
  std::uint32_t flags = 0;
  std::memcpy(&flags, static_cast<const std::uint8_t*>(social) + kFacadeFlagsOffset, sizeof(flags));
  QueueJsonTrace(JsonTraceKind::kSocialJson, callCount, nullptr, flags,
                 reinterpret_cast<std::uintptr_t>(result), JsonWord(json, 0), JsonWord(json, 8));
  return result;
}

std::uint32_t JsonSetHook(void* json, const char* path, std::uint32_t required) {
  const JsonSetFn original = g_originalJsonSet.load(std::memory_order_acquire);
  if (original == nullptr || original == &JsonSetHook ||
      reinterpret_cast<void*>(original) == g_jsonSetTarget.load(std::memory_order_acquire)) return 0;
  const std::uint32_t result = original(json, path, required);
  if (json != g_facadeJson.load(std::memory_order_acquire)) return result;
  const std::uint32_t callCount = CountTrace(g_jsonSetCalls);
  if (ShouldLog(callCount)) {
    QueueJsonTrace(JsonTraceKind::kSet, callCount, path != nullptr ? path : "<null>", required, result,
                   JsonWord(json, 0), JsonWord(json, 8));
  }
  return result;
}

void* JsonNavigateForWriteHook(const char* path, void* json, void* outA, void* outB) {
  const JsonNavigateForWriteFn original = g_originalJsonNavigateForWrite.load(std::memory_order_acquire);
  if (original == nullptr || original == &JsonNavigateForWriteHook ||
      reinterpret_cast<void*>(original) == g_jsonNavigateForWriteTarget.load(std::memory_order_acquire)) return nullptr;
  void* result = original(path, json, outA, outB);
  if (json != g_facadeJson.load(std::memory_order_acquire)) return result;
  const std::uint32_t callCount = CountTrace(g_jsonNavigateForWriteCalls);
  if (ShouldLog(callCount)) {
    QueueJsonTrace(JsonTraceKind::kNavigateForWrite, callCount, path != nullptr ? path : "<null>", 0,
                   reinterpret_cast<std::uintptr_t>(result), JsonWord(json, 0), JsonWord(json, 8));
  }
  return result;
}

template <typename Fn, std::size_t Size>
void InstallChecked(std::uintptr_t gameBase, std::uint64_t virtualAddress,
                    const std::array<std::uint8_t, Size>& prologue, Fn& original, PVOID detour,
                    const char* detourName, const char* probeName) {
  void* target = nevr::ResolveVA_Checked(gameBase, virtualAddress);
  if (!nevr::ValidatePrologue(target, prologue.data(), prologue.size())) {
    Log(EchoVR::LogLevel::Warning, "[NEVR.SOCIAL] %s probe skipped va=0x%llx reason=prologue_mismatch",
        probeName, static_cast<unsigned long long>(virtualAddress));
    return;
  }
  original = reinterpret_cast<Fn>(target);
  PatchDetour(&original, detour, detourName);
}

template <typename Fn, std::size_t Size>
void InstallJsonProbe(std::uint64_t virtualAddress, const std::array<std::uint8_t, Size>& prologue,
                      std::atomic<Fn>& original, std::atomic<void*>& targetSlot,
                      PVOID detour, const char* probeName) {
  void* target = nevr::ResolveVA_Checked(g_gameBase, virtualAddress);
  if (!nevr::ValidatePrologue(target, prologue.data(), prologue.size())) {
    Log(EchoVR::LogLevel::Warning, "[NEVR.SOCIAL] %s probe skipped va=0x%llx reason=prologue_mismatch",
        probeName, static_cast<unsigned long long>(virtualAddress));
    return;
  }
  targetSlot.store(target, std::memory_order_release);
  MH_STATUS createStatus = MH_OK;
  MH_STATUS enableStatus = MH_OK;
  MH_STATUS removeStatus = MH_OK;
  bool created = false;
  const bool enabled = Hooking::CreatePublishEnable(
      [&](void** trampoline) {
        createStatus = MH_CreateHook(target, detour, trampoline);
        created = createStatus == MH_OK;
        if (!created) return false;
        if (*trampoline == nullptr || *trampoline == target || *trampoline == detour) {
          removeStatus = MH_RemoveHook(target);
          created = false;
          return false;
        }
        return true;
      },
      [&](void* trampoline) { original.store(reinterpret_cast<Fn>(trampoline), std::memory_order_release); },
      [&] {
        enableStatus = MH_EnableHook(target);
        return enableStatus == MH_OK;
      });
  if (enabled) return;
  if (created) {
    removeStatus = MH_RemoveHook(target);
    if (removeStatus == MH_OK) original.store(nullptr, std::memory_order_release);
  }
  Log(EchoVR::LogLevel::Warning, "[NEVR.SOCIAL] %s probe skipped va=0x%llx create=%s enable=%s remove=%s",
      probeName, static_cast<unsigned long long>(virtualAddress), MH_StatusToString(createStatus),
      MH_StatusToString(enableStatus), MH_StatusToString(removeStatus));
}

void LogJsonOps();

void EnsureJsonHooksInstalled(void* facadeObject) {
  std::call_once(g_jsonHooksOnce, [facadeObject] {
    LogJsonOps();
    g_facadeObject.store(facadeObject, std::memory_order_release);
    g_facadeJson.store(static_cast<std::uint8_t*>(facadeObject) + kFacadeJsonOffset,
                       std::memory_order_release);
    InstallHookPlan(InstallStage::kFacadeSelected, true, [](Probe probe) {
      switch (probe) {
        case Probe::kSocialJson:
          InstallJsonProbe(kSocialJsonVA, kSocialJsonPrologue, g_originalSocialJson, g_socialJsonTarget,
                           reinterpret_cast<PVOID>(&SocialJsonHook), "social_json");
          break;
        case Probe::kJsonSet:
          InstallJsonProbe(kJsonSetVA, kJsonSetPrologue, g_originalJsonSet, g_jsonSetTarget,
                           reinterpret_cast<PVOID>(&JsonSetHook), "json_set");
          break;
        case Probe::kJsonNavigateForWrite:
          InstallJsonProbe(kJsonNavigateForWriteVA, kJsonNavigateForWritePrologue, g_originalJsonNavigateForWrite,
                           g_jsonNavigateForWriteTarget,
                           reinterpret_cast<PVOID>(&JsonNavigateForWriteHook), "json_navigate_write");
          break;
        case Probe::kAccessor: break;
      }
    });
    // Probe installation is intentionally one-shot. Each mismatch/attach failure
    // is logged independently; any successfully installed probes remain active.
  });
}

// The CJson functions the party data sync calls (social_facade.h JsonOps), first 16 bytes each from
// ReVault's disassembly of echovr.exe.
constexpr std::uint64_t kJsonLoadVA = 0x1405F0BD0;
constexpr std::uint64_t kJsonClearVA = 0x1405ECE60;
constexpr std::uint64_t kJsonSerializeVA = 0x1405F1DC0;
constexpr std::uint64_t kMemBlockResetVA = 0x1400D4E50;
constexpr std::uint64_t kMemBlockDestroyVA = 0x1400D2760;
constexpr std::array<std::uint8_t, 16> kJsonLoadPrologue = {0x48, 0x89, 0x5c, 0x24, 0x08, 0x48, 0x89, 0x6c,
                                                            0x24, 0x10, 0x56, 0x57, 0x41, 0x56, 0x48, 0x81};
constexpr std::array<std::uint8_t, 16> kJsonClearPrologue = {0x40, 0x53, 0x48, 0x83, 0xec, 0x20, 0x48, 0x8b,
                                                             0xd9, 0xe8, 0x62, 0xff, 0x00, 0x00, 0x41, 0xb8};
constexpr std::array<std::uint8_t, 16> kJsonSerializePrologue = {0x48, 0x89, 0x5c, 0x24, 0x08, 0x48, 0x89, 0x6c,
                                                                 0x24, 0x18, 0x48, 0x89, 0x54, 0x24, 0x10, 0x56};
constexpr std::array<std::uint8_t, 16> kMemBlockResetPrologue = {0x83, 0x61, 0x1c, 0xfe, 0x33, 0xc0, 0x48, 0x89,
                                                                 0x01, 0x48, 0x89, 0x41, 0x08, 0x89, 0x41, 0x18};
constexpr std::array<std::uint8_t, 16> kMemBlockDestroyPrologue = {0x40, 0x53, 0x48, 0x83, 0xec, 0x20, 0x48, 0x8b,
                                                                   0xd9, 0x48, 0x83, 0x79, 0x08, 0x00, 0x74, 0x39};

template <typename Fn>
Fn ResolveCall(std::uintptr_t gameBase, std::uint64_t va, const std::array<std::uint8_t, 16>& prologue) {
  void* target = nevr::ResolveVA_Checked(gameBase, va);
  if (!nevr::ValidatePrologue(target, prologue.data(), prologue.size())) return nullptr;
  return reinterpret_cast<Fn>(target);
}

JsonOps g_resolvedJsonOps;  // what Install found, logged once the game's log exists (LogJsonOps)

// Runs inside Install, during DLL initialisation, before the game's logger exists: no Log here.
void InstallJsonOps(std::uintptr_t gameBase) {
  JsonOps ops;
  ops.load = ResolveCall<decltype(ops.load)>(gameBase, kJsonLoadVA, kJsonLoadPrologue);
  ops.clear = ResolveCall<decltype(ops.clear)>(gameBase, kJsonClearVA, kJsonClearPrologue);
  ops.serialize = ResolveCall<decltype(ops.serialize)>(gameBase, kJsonSerializeVA, kJsonSerializePrologue);
  ops.blockReset = ResolveCall<decltype(ops.blockReset)>(gameBase, kMemBlockResetVA, kMemBlockResetPrologue);
  ops.blockDestroy = ResolveCall<decltype(ops.blockDestroy)>(gameBase, kMemBlockDestroyVA, kMemBlockDestroyPrologue);
  g_resolvedJsonOps = ops;
  SetJsonOps(ops);
}

void LogJsonOps() {
  const JsonOps& ops = g_resolvedJsonOps;
  const bool all = ops.load != nullptr && ops.clear != nullptr && ops.serialize != nullptr && ops.blockReset != nullptr &&
                   ops.blockDestroy != nullptr;
  Log(all ? EchoVR::LogLevel::Info : EchoVR::LogLevel::Warning,
      "[NEVR.SOCIAL] party data json functions load=%d clear=%d serialize=%d memblock=%d/%d (prologue checked)%s",
      ops.load != nullptr, ops.clear != nullptr, ops.serialize != nullptr, ops.blockReset != nullptr,
      ops.blockDestroy != nullptr, all ? "" : ": party data off");
}

}  // namespace

void Install(std::uintptr_t gameBase) {
  g_gameBase = gameBase;
  InstallJsonOps(gameBase);
  g_providerName = reinterpret_cast<ProviderNameFn>(nevr::ResolveVA_Checked(gameBase, kProviderNameVA));
  InstallHookPlan(InstallStage::kBoot, false, [gameBase](Probe probe) {
    if (probe == Probe::kAccessor) {
      InstallChecked(gameBase, kAccessorVA, kAccessorPrologue, g_originalAccessor,
                     reinterpret_cast<PVOID>(&AccessorHook), "SocialAccessor", "accessor");
    }
  });
}

}  // namespace SocialFacade
