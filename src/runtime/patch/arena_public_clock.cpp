#include "runtime/patch/arena_public_clock.h"

#include <winsock2.h>
#include <windows.h>

#include <MinHook.h>

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

// One entry per kSites row. `original` is what the detour calls after its decision, read on a script thread with an
// acquire load and written only by __atomic_store_n (Hooking::Attach publishes the trampoline into it before it
// enables the hook, core/hook_lifecycle.h). Once a site has been hooked it is never null and never a freed
// trampoline: it holds the game's bind function (`target`) whenever no live trampoline exists. `target` and
// `lastReport` are guarded by g_mutex.
struct SiteHook {
  void* original{nullptr};
  void* target{nullptr};
  std::uintptr_t lastReportBase{0};
  SiteState lastReportState{SiteState::Unhooked};
  bool reported{false};
};

std::mutex g_mutex;
SiteHook g_hooks[kSiteCount];
bool g_installed = false;

// The two MinHook calls the hook makes, behind pointers so a test can stand in for them (RefreshForTest needs the
// stock script DLLs; the attach window and the removal do not).
bool DefaultAttach(void** original, void* detour) { return Hooking::Attach(original, detour) != FALSE; }
int DefaultRemove(void* target) { return MH_RemoveHook(target); }
struct Backend {
  bool (*attach)(void** original, void* detour);
  int (*remove)(void* target);
};
Backend g_backend{&DefaultAttach, &DefaultRemove};

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
  const auto original = reinterpret_cast<BindFn>(__atomic_load_n(&g_hooks[I].original, __ATOMIC_ACQUIRE));
  return original(state, node);
}

template <std::size_t... Is>
constexpr std::array<void*, kSiteCount> DetourTable(std::index_sequence<Is...>) {
  return {{reinterpret_cast<void*>(&BindDetour<Is>)...}};
}

const std::array<void*, kSiteCount>& Detours() {
  static const std::array<void*, kSiteCount> detours = DetourTable(std::make_index_sequence<kSiteCount>{});
  return detours;
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

// Hooks `target` for site `i`. Caller holds g_mutex.
//
// The slot the detour reads is the one handed to Attach, so the trampoline is in it before the hook is enabled, and a
// failed attach puts back what it held on entry: the target, which is the game's own function and so a valid thing
// for a late caller to reach. An earlier hook at this site lives on in MinHook's table (the image was released and
// mapped again, maybe at the same base): it is removed first, or MH_CreateHook answers MH_ERROR_ALREADY_CREATED. Its
// trampoline dies with it, and the slot takes the new target on the next line, so a caller that reaches the detour
// after that finds the game's function. (MinHook frees the trampoline inside MH_RemoveHook; a thread that is past the
// detour's load of the slot, or that arrives between that call and the store, is outside what this layer can order.)
// A removal that failed leaves the old hook and its trampoline alive: it is logged, the slot is left alone until
// the new target replaces it, and the next pass removes the old hook again.
bool HookSite(std::size_t i, void* target) {
  SiteHook& hook = g_hooks[i];
  if (hook.target != nullptr) {
    const int status = g_backend.remove(hook.target);
    if (status == MH_OK || status == MH_ERROR_NOT_CREATED) {
      hook.target = nullptr;
    } else {
      Log(EchoVR::LogLevel::Warning,
          "[NEVR.PATCH] arena_public_clock could not remove the earlier hook at %p: %s; it is tried again on the next load",
          hook.target, MH_StatusToString(static_cast<MH_STATUS>(status)));
    }
  }
  __atomic_store_n(&hook.original, target, __ATOMIC_RELEASE);
  if (!g_backend.attach(&hook.original, Detours()[i])) return false;
  hook.target = target;
  return true;
}

// Runs after every LoadLibrary the hooks see. A script DLL that is mapped and still carries the shipped bytes is
// hooked: the first load, and every reload after the game released the script (the new image is fresh, and a hook
// left on the old mapping is gone with it). This is a raw Hooking::Attach, not PatchDetour: HookGuard records one
// site per call and has no retire, so a hook per reload would fill its table and report each fresh image as an
// overwrite.
void Refresh() {
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
    void* target = const_cast<std::uint8_t*>(image) + site.bindRva;
    if (!HookSite(i, target)) {
      Log(EchoVR::LogLevel::Warning,
          "[NEVR.PATCH] arena_public_clock hook FAILED script=%s target=%p reason=%s; the script keeps the game's "
          "behaviour",
          site.dll, target, Hooking::LastAttachError());
      continue;
    }
    Report(hook, site, base, state, "hooked");
  }
}

}  // namespace

#ifdef NEVR_ARENA_PUBLIC_CLOCK_TESTING
void RefreshForTest() { Refresh(); }
BindFn OriginalForTest(std::size_t site) {
  return reinterpret_cast<BindFn>(__atomic_load_n(&g_hooks[site].original, __ATOMIC_ACQUIRE));
}
void SetOriginalForTest(std::size_t site, BindFn original) {
  __atomic_store_n(&g_hooks[site].original, reinterpret_cast<void*>(original), __ATOMIC_RELEASE);
}
void SetBackendForTest(AttachFn attach, RemoveFn remove) {
  g_backend = Backend{attach != nullptr ? attach : &DefaultAttach, remove != nullptr ? remove : &DefaultRemove};
}
bool HookSiteForTest(std::size_t site, void* target) {
  std::lock_guard<std::mutex> lock(g_mutex);
  return HookSite(site, target);
}
BindFn DetourForTest(std::size_t site) { return reinterpret_cast<BindFn>(Detours()[site]); }
void ResetForTest() {
  std::lock_guard<std::mutex> lock(g_mutex);
  for (SiteHook& hook : g_hooks) {
    __atomic_store_n(&hook.original, static_cast<void*>(nullptr), __ATOMIC_RELEASE);
    hook.target = nullptr;
  }
  g_backend = Backend{&DefaultAttach, &DefaultRemove};
}
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
