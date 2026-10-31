#include "runtime/patch/provider_name_redirect.h"

#include <winsock2.h>
#include <windows.h>

#include <cstring>
#include <string>

#include "core/logging.h"
#include "nevr/pc/nevr_common.h"  // N97: the one ValidatePrologue
#include "runtime/hook/addresses.h"
#include "runtime/hook/patching.h"

// The loader's address and 24-byte prologue are CNSPROVIDER_LOAD_BY_NAME and CNSPROVIDER_LOAD_BY_NAME_PROLOGUE in
// runtime/hook/addresses.h.
namespace nevr_provider_name_redirect {
namespace {

constexpr char kRadName[] = "pnsrad";

using LoadByNameFn = std::uint64_t (*)(void* slot, const char* name, void* cfg, void* out);

LoadByNameFn g_original = nullptr;

bool IsStockPlatformName(const char* name) {
  return name != nullptr && (std::strcmp(name, "pnsovr") == 0 || std::strcmp(name, "pnsdemo") == 0);
}

std::uint64_t LoadByNameHook(void* slot, const char* name, void* cfg, void* out) {
  if (!IsStockPlatformName(name)) return g_original(slot, name, cfg, out);
  const std::uint64_t result = g_original(slot, kRadName, cfg, out);
  Log(EchoVR::LogLevel::Info,
      "[NEVR.PATCH] provider name redirected from=%s to=pnsrad slot=%p result=%llu (the game's own call named a "
      "stock social module)",
      name, slot, static_cast<unsigned long long>(result));
  return result;
}

std::string HexBytes(const std::uint8_t* bytes, std::size_t len) {
  static const char kDigits[] = "0123456789abcdef";
  std::string out;
  out.reserve(len * 2);
  for (std::size_t i = 0; i < len; ++i) {
    out.push_back(kDigits[bytes[i] >> 4]);
    out.push_back(kDigits[bytes[i] & 0x0F]);
  }
  return out;
}

}  // namespace

bool InstallAt(void* target, const std::uint8_t* expected, std::size_t expectedLen) {
  if (!nevr::ValidatePrologue(target, expected, expectedLen)) {
    const std::string actual =
        target != nullptr ? HexBytes(static_cast<const std::uint8_t*>(target), expectedLen) : std::string("unmapped");
    Log(EchoVR::LogLevel::Warning,
        "[NEVR.PATCH] provider name redirect not installed reason=prologue_mismatch name=CNSProvider::LoadByName "
        "va=%p expected=%s actual=%s (pnsovr and pnsdemo keep loading the stock social module)",
        target, HexBytes(expected, expectedLen).c_str(), actual.c_str());
    return false;
  }
  g_original = reinterpret_cast<LoadByNameFn>(target);
  if (!PatchDetour(&g_original, reinterpret_cast<PVOID>(&LoadByNameHook), "CNSProvider::LoadByName")) {
    return false;
  }
  Log(EchoVR::LogLevel::Info, "[NEVR.PATCH] provider name redirect installed va=%p (pnsovr and pnsdemo now load pnsrad)", target);
  return true;
}

bool Install(std::uintptr_t gameBase) {
  using namespace nevr_patch_addresses;
  // ResolveVA_Checked takes a full VA and is null when nothing is committed there; InstallAt then refuses.
  void* target = nevr::ResolveVA_Checked(gameBase, nevr::ECHOVR_DEFAULT_IMAGE_BASE + CNSPROVIDER_LOAD_BY_NAME);
  return InstallAt(target, CNSPROVIDER_LOAD_BY_NAME_PROLOGUE, sizeof(CNSPROVIDER_LOAD_BY_NAME_PROLOGUE));
}

}  // namespace nevr_provider_name_redirect
