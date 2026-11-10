#include "runtime/hook/script_resource_hook.h"

#include <winsock2.h>
#include <windows.h>

#include <atomic>
#include <cstring>
#include <string>

#include <MinHook.h>

#include "core/logging.h"
#include "nevr/pc/nevr_common.h"
#include "runtime/hook/patching.h"
#include "runtime/hook/script_hooks.h"

namespace nevr_script_resource_hook {
namespace {

using LoadFn = std::uint64_t (*)(void* resource);
using UnloadFn = void (*)(void* resource);

LoadFn g_load = nullptr;
UnloadFn g_unload = nullptr;
std::atomic_bool g_installed{false};

std::uint64_t Name(void* resource) {
  std::uint64_t v = 0;
  std::memcpy(&v, static_cast<std::uint8_t*>(resource) + kNameOffset, sizeof v);
  return v;
}

void* Module(void* resource) {
  void* v = nullptr;
  std::memcpy(&v, static_cast<std::uint8_t*>(resource) + kModuleOffset, sizeof v);
  return v;
}

std::uint64_t LoadHook(void* resource) {
  const std::uint64_t result = g_load(resource);
  AfterLoad(resource, result);
  return result;
}

void UnloadHook(void* resource) {
  BeforeUnload(resource);
  g_unload(resource);
}

std::string Hex(const std::uint8_t* p, std::size_t n) {
  static const char kDigits[] = "0123456789abcdef";
  std::string out;
  for (std::size_t i = 0; i < n; ++i) {
    out += kDigits[p[i] >> 4];
    out += kDigits[p[i] & 0xf];
  }
  return out;
}

// Rule 6: the address, the bytes expected and the bytes found.
bool Checked(void* target, std::uint64_t va, const std::uint8_t* expected, std::size_t n, const char* name) {
  if (target != nullptr && nevr::ValidatePrologue(target, expected, n)) return true;
  Log(EchoVR::LogLevel::Warning,
      "[NEVR.SCRIPT] hook not installed name=\"%s\" va=0x%llx expected=%s actual=%s (the game build is not the one this "
      "hook was written against)",
      name, static_cast<unsigned long long>(va), Hex(expected, n).c_str(),
      target != nullptr ? Hex(static_cast<const std::uint8_t*>(target), n).c_str() : "unmapped");
  return false;
}

}  // namespace

void AfterLoad(void* resource, std::uint64_t result) {
  if (result != 0) return;  // a failed load holds no module the hooks could use
  nevr_script_hooks::OnLoaded(Name(resource), Module(resource));
}

void BeforeUnload(void* resource) { nevr_script_hooks::OnUnloading(Name(resource), Module(resource)); }

void Install(std::uintptr_t game_base) {
  if (g_installed.exchange(true)) return;

  void* load = nevr::ResolveVA_Checked(game_base, kLoadVa);
  void* unload = nevr::ResolveVA_Checked(game_base, kUnloadVa);
  const bool load_ok = Checked(load, kLoadVa, kLoadPrologue, sizeof kLoadPrologue, "CScriptResource::Load");
  const bool unload_ok = Checked(unload, kUnloadVa, kUnloadPrologue, sizeof kUnloadPrologue, "CScriptResource free");
  bool ok = load_ok && unload_ok;
  if (ok) {
    g_load = reinterpret_cast<LoadFn>(load);
    g_unload = reinterpret_cast<UnloadFn>(unload);
    // Both or neither: a load hook without the free hook would leave detours in freed scripts.
    if (!PatchDetour(&g_load, reinterpret_cast<PVOID>(&LoadHook), "CScriptResource::Load")) {
      ok = false;
    } else if (!PatchDetour(&g_unload, reinterpret_cast<PVOID>(&UnloadHook), "CScriptResource free")) {
      const MH_STATUS st = PatchRetire(load);
      if (st != MH_OK) {
        Log(EchoVR::LogLevel::Warning,
            "[NEVR.SCRIPT] CScriptResource::Load hook not removed status=%s (scripts would be hooked and never unhooked)",
            MH_StatusToString(st));
      }
      ok = false;
    }
  }
  if (ok) {
    Log(EchoVR::LogLevel::Info,
        "[NEVR.SCRIPT] script load hooks installed load_va=0x%llx free_va=0x%llx (each script's hooks go in as the game "
        "loads it and come out before it frees it)",
        static_cast<unsigned long long>(kLoadVa), static_cast<unsigned long long>(kUnloadVa));
  } else {
    Log(EchoVR::LogLevel::Warning,
        "[NEVR.SCRIPT] script load hooks not installed load_bytes=%s free_bytes=%s (no script is hooked; every script runs "
        "stock; a line above names the cause)",
        load_ok ? "match" : "differ", unload_ok ? "match" : "differ");
  }
}

#ifdef NEVR_TEST_HOOKS
void ResetForTest() {
  g_installed = false;
  g_load = nullptr;
  g_unload = nullptr;
}
#endif

}  // namespace nevr_script_resource_hook
