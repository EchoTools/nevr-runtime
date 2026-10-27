#include "runtime/patch/arena_public_clock.h"

#include <winsock2.h>
#include <windows.h>

#include <MinHook.h>

#include <atomic>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <utility>

#include "runtime/lifecycle/cli.h"
#include "core/hooking.h"
#include "core/logging.h"
#include "runtime/hook/dll_load_hook.h"
#include "runtime/lifecycle/arena_round_time.h"
#include "runtime/patch/arena_public_clock_rules.h"

namespace nevr_arena_public_clock {
namespace {

using BindFn = std::int64_t (*)(void* state, void* node);

// One entry per kSites row. `original` is the trampoline, read by the detour on a script thread; `target` and
// `lastReport` are guarded by g_mutex.
struct SiteHook {
  std::atomic<BindFn> original{nullptr};
  void* target{nullptr};
  std::uintptr_t lastReportBase{0};
  SiteState lastReportState{SiteState::Unhooked};
  bool reported{false};
};

std::mutex g_mutex;
SiteHook g_hooks[kSiteCount];
bool g_installed = false;

template <std::size_t I>
std::int64_t BindDetour(void* state, void* node) {
  // Only the public-match flag store is looked at; every other R15NETSETFLAG node of the script passes straight
  // through with one id compare.
  const std::optional<std::uint64_t> id = StateFlagId(static_cast<const std::uint8_t*>(state));
  if (id && *id == kPublicMatchFlag) {
    const std::optional<std::string> gameType = nevr_arena_round_time::SessionGameType();
    const FlagStoreOutcome outcome =
        ApplyToFlagState(static_cast<std::uint8_t*>(state), g_isServer != FALSE, gameType);
    Log(EchoVR::LogLevel::Info,
        "[NEVR.PATCH] arena_public_clock applied=%d reason=%s game_type=%s flag=0x%016llx script=%s value_before=%d "
        "value_after=%d",
        outcome.decision.forceFalse ? 1 : 0, std::string(outcome.decision.reason).c_str(),
        gameType ? gameType->c_str() : "none", static_cast<unsigned long long>(kPublicMatchFlag), kSites[I].dll,
        outcome.valueBefore, outcome.valueAfter);
  }
  return g_hooks[I].original.load()(state, node);
}

template <std::size_t... Is>
constexpr std::array<void*, kSiteCount> DetourTable(std::index_sequence<Is...>) {
  return {{reinterpret_cast<void*>(&BindDetour<Is>)...}};
}

std::size_t ImageSize(HMODULE module) {
  const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(module);
  if (dos->e_magic != IMAGE_DOS_SIGNATURE) return 0;
  const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(reinterpret_cast<const std::uint8_t*>(module) + dos->e_lfanew);
  if (nt->Signature != IMAGE_NT_SIGNATURE) return 0;
  return nt->OptionalHeader.SizeOfImage;
}

// Logs a site's state once per (module base, state), so a LoadLibrary-heavy second does not repeat a line.
void Report(SiteHook& hook, const ScriptSite& site, std::uintptr_t base, SiteState state, const char* verb) {
  if (hook.reported && hook.lastReportBase == base && hook.lastReportState == state) return;
  hook.reported = true;
  hook.lastReportBase = base;
  hook.lastReportState = state;
  const bool bad = state != SiteState::Unhooked && state != SiteState::Hooked;
  Log(bad ? EchoVR::LogLevel::Warning : EchoVR::LogLevel::Info,
      "[NEVR.PATCH] arena_public_clock script=%s module=%p state=%s %s", site.dll, reinterpret_cast<void*>(base),
      SiteStateName(state), verb);
}

// Runs after every LoadLibrary the hooks see. A script DLL that is mapped and still carries the shipped bytes is
// hooked: the first load, and every reload after the game released the script (the new image is fresh, and a hook
// left on the old mapping is gone with it). This is a raw Hooking::Attach, not PatchDetour: HookGuard records one
// site per call and has no retire, so a hook per reload would fill its table and report each fresh image as an
// overwrite.
void Refresh() {
  static const std::array<void*, kSiteCount> detours = DetourTable(std::make_index_sequence<kSiteCount>{});
  std::lock_guard<std::mutex> lock(g_mutex);
  for (std::size_t i = 0; i < kSiteCount; ++i) {
    const ScriptSite& site = kSites[i];
    SiteHook& hook = g_hooks[i];
    HMODULE module = GetModuleHandleA(site.dll);
    if (module == nullptr) continue;
    const auto* image = reinterpret_cast<const std::uint8_t*>(module);
    const SiteState state = ClassifySite(image, ImageSize(module), site);
    const auto base = reinterpret_cast<std::uintptr_t>(module);
    if (state == SiteState::Hooked) {
      Report(hook, site, base, state, "already hooked");
      continue;
    }
    if (state != SiteState::Unhooked) {
      Report(hook, site, base, state, "not hooked; the script keeps the game's behaviour");
      continue;
    }
    // Fresh bytes. An earlier hook at this address lives on in MinHook's table (the image was released and mapped
    // again, maybe at the same base): remove it first, or MH_CreateHook answers MH_ERROR_ALREADY_CREATED.
    if (hook.target != nullptr) {
      MH_RemoveHook(hook.target);
      hook.target = nullptr;
    }
    void* target = const_cast<std::uint8_t*>(image) + site.bindRva;
    BindFn trampoline = reinterpret_cast<BindFn>(target);
    if (!Hooking::Attach(reinterpret_cast<PVOID*>(&trampoline), detours[i])) {
      Log(EchoVR::LogLevel::Warning,
          "[NEVR.PATCH] arena_public_clock hook FAILED script=%s target=%p reason=%s; the script keeps the game's "
          "behaviour",
          site.dll, target, Hooking::LastAttachError());
      continue;
    }
    hook.original.store(trampoline);
    hook.target = target;
    Report(hook, site, base, state, "hooked");
  }
}

}  // namespace

#ifdef NEVR_ARENA_PUBLIC_CLOCK_TESTING
void RefreshForTest() { Refresh(); }
BindFn OriginalForTest(std::size_t site) { return g_hooks[site].original.load(); }
void SetOriginalForTest(std::size_t site, BindFn original) { g_hooks[site].original.store(original); }
#endif

void Install() {
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_installed) return;
    g_installed = true;
  }
  nevr_dll_load_hook::AddPostLoadObserver(&Refresh);
  Refresh();  // a script already loaded when the server boots
  Log(EchoVR::LogLevel::Info, "[NEVR.PATCH] arena_public_clock armed for %d script DLLs", static_cast<int>(kSiteCount));
}

}  // namespace nevr_arena_public_clock
