#include "runtime/patch/social_facade.h"

#include <windows.h>

#include <array>
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
constexpr std::uintptr_t kNetGameOffset = 0x8518;
constexpr std::uintptr_t kSocialDisabledOffset = 0x647E9;
constexpr std::array<std::uint8_t, 18> kAccessorPrologue = {0x48, 0x89, 0x5C, 0x24, 0x18, 0x48, 0x89, 0x74, 0x24,
                                                            0x20, 0x57, 0x48, 0x81, 0xEC, 0x30, 0x02, 0x00, 0x00};

using AccessorFn = void* (*)(void* provider);
using ProviderNameFn = void (*)(void* provider, char* name);
AccessorFn g_originalAccessor = nullptr;
ProviderNameFn g_providerName = nullptr;
std::uint32_t g_accessorCalls = 0;

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

}  // namespace

void Install(std::uintptr_t gameBase) {
  void* target = nevr::ResolveVA_Checked(gameBase, kAccessorVA);
  if (!nevr::ValidatePrologue(target, kAccessorPrologue.data(), kAccessorPrologue.size())) {
    Log(EchoVR::LogLevel::Warning, "[NEVR.SOCIAL] accessor probe skipped va=0x%llx reason=prologue_mismatch",
        static_cast<unsigned long long>(kAccessorVA));
    return;
  }
  g_providerName = reinterpret_cast<ProviderNameFn>(nevr::ResolveVA_Checked(gameBase, kProviderNameVA));
  g_originalAccessor = reinterpret_cast<AccessorFn>(target);
  PatchDetour(&g_originalAccessor, reinterpret_cast<PVOID>(&AccessorHook), "SocialAccessor");
}

}  // namespace SocialFacade
