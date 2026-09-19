#include "runtime/patch/social_facade.h"

#include <windows.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>

#include "abi/echovr_functions.h"
#include "core/logging.h"
#include "nevr_common.h"
#include "runtime/hook/patching.h"
#include "runtime/lifecycle/config.h"
#include "runtime/lifecycle/service_config.h"

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
SocialJsonFn g_originalSocialJson = nullptr;
JsonSetFn g_originalJsonSet = nullptr;
JsonNavigateForWriteFn g_originalJsonNavigateForWrite = nullptr;
void* g_facadeObject = nullptr;
void* g_facadeJson = nullptr;
std::uint32_t g_accessorCalls = 0;
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

void* AccessorHook(void* provider) {
  void* result = g_originalAccessor(provider);
  char providerName[512] = "<unresolved>";
  if (provider != nullptr && g_providerName != nullptr) g_providerName(provider, providerName);
  providerName[sizeof(providerName) - 1] = '\0';
  const bool enabled = NevrCfgSocialFacadeEnabled();
  void* returned = (enabled && result == nullptr) ? Object() : result;
  ++g_accessorCalls;
  Log(EchoVR::LogLevel::Info,
      "[NEVR.SOCIAL] accessor provider=%s provider_ptr=%p original=%p returned=%p "
      "disabled_byte=%d facade_enabled=%d call_count=%u",
      providerName, provider, result, returned, SocialDisabledByte(), enabled ? 1 : 0, g_accessorCalls);
  return returned;
}

void* SocialJsonHook(void* social) {
  void* result = g_originalSocialJson(social);
  if (social != g_facadeObject) return result;
  const std::uint32_t callCount = CountTrace(g_socialJsonCalls);
  if (!ShouldLog(callCount)) return result;
  std::uint32_t flags = 0;
  std::memcpy(&flags, static_cast<const std::uint8_t*>(social) + kFacadeFlagsOffset, sizeof(flags));
  Log(EchoVR::LogLevel::Info,
      "[NEVR.SOCIAL] social_json call_count=%u returned=%p embedded=%d flags=0x%x root=%p cache=%p",
      callCount, result, result == g_facadeJson ? 1 : 0, flags,
      reinterpret_cast<void*>(JsonWord(g_facadeJson, 0)), reinterpret_cast<void*>(JsonWord(g_facadeJson, 8)));
  return result;
}

std::uint32_t JsonSetHook(void* json, const char* path, std::uint32_t required) {
  const std::uint32_t result = g_originalJsonSet(json, path, required);
  if (json != g_facadeJson) return result;
  const std::uint32_t callCount = CountTrace(g_jsonSetCalls);
  if (ShouldLog(callCount)) {
    Log(EchoVR::LogLevel::Info,
        "[NEVR.SOCIAL] json_set call_count=%u path=%s required=%u result=%u root=%p cache=%p",
        callCount, path != nullptr ? path : "<null>", required, result,
        reinterpret_cast<void*>(JsonWord(json, 0)), reinterpret_cast<void*>(JsonWord(json, 8)));
  }
  return result;
}

void* JsonNavigateForWriteHook(const char* path, void* json, void* outA, void* outB) {
  void* result = g_originalJsonNavigateForWrite(path, json, outA, outB);
  if (json != g_facadeJson) return result;
  const std::uint32_t callCount = CountTrace(g_jsonNavigateForWriteCalls);
  if (ShouldLog(callCount)) {
    Log(EchoVR::LogLevel::Info,
        "[NEVR.SOCIAL] json_navigate_write call_count=%u path=%s result=%p root=%p cache=%p",
        callCount, path != nullptr ? path : "<null>", result,
        reinterpret_cast<void*>(JsonWord(json, 0)), reinterpret_cast<void*>(JsonWord(json, 8)));
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

}  // namespace

void Install(std::uintptr_t gameBase) {
  g_facadeObject = Object();
  g_facadeJson = static_cast<std::uint8_t*>(g_facadeObject) + kFacadeJsonOffset;
  g_providerName = reinterpret_cast<ProviderNameFn>(nevr::ResolveVA_Checked(gameBase, kProviderNameVA));
  InstallChecked(gameBase, kAccessorVA, kAccessorPrologue, g_originalAccessor,
                 reinterpret_cast<PVOID>(&AccessorHook), "SocialAccessor", "accessor");
  InstallChecked(gameBase, kSocialJsonVA, kSocialJsonPrologue, g_originalSocialJson,
                 reinterpret_cast<PVOID>(&SocialJsonHook), "SocialJson", "social_json");
  InstallChecked(gameBase, kJsonSetVA, kJsonSetPrologue, g_originalJsonSet,
                 reinterpret_cast<PVOID>(&JsonSetHook), "SocialJsonSet", "json_set");
  InstallChecked(gameBase, kJsonNavigateForWriteVA, kJsonNavigateForWritePrologue,
                 g_originalJsonNavigateForWrite, reinterpret_cast<PVOID>(&JsonNavigateForWriteHook),
                 "SocialJsonNavigateForWrite", "json_navigate_write");
}

}  // namespace SocialFacade
