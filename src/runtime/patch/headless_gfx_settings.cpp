#include "runtime/patch/headless_gfx_settings.h"

#include <winsock2.h>
#include <windows.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <string>

#include "core/logging.h"
#include "nevr/pc/nevr_common.h"
#include "runtime/hook/hook_liveness.h"
#include "runtime/hook/patching.h"
#include "runtime/patch/render_config.h"

namespace nevr_headless_gfx_settings {
namespace {

using ApplyFn = std::uint64_t (*)(void* settings, void* game, int logFlag);

// The trampoline PatchDetour writes, and whether the detour went in. The hook never calls the trampoline.
ApplyFn g_original = nullptr;
bool g_installed = false;
std::atomic<std::uint64_t> g_calls{0};

std::string HexBytes(const void* address, std::size_t count) {
  std::string out;
  char byte[4];
  for (std::size_t i = 0; i < count; ++i) {
    std::snprintf(byte, sizeof(byte), "%02x", static_cast<const std::uint8_t*>(address)[i]);
    out += byte;
  }
  return out;
}

// noinline: the hook is entered from the game's own call sites, and the first line runs on the game's thread.
__attribute__((noinline)) std::uint64_t ApplyHook(void*, void*, int) {
  nevr_hook_liveness::Mark(nevr_hook_liveness::kApplyGfxSettings);
  const std::uint64_t calls = g_calls.fetch_add(1, std::memory_order_relaxed) + 1;
  // The first three calls, then every hundredth, so a settings change never floods the log.
  if (calls <= 3 || calls % 100 == 0) {
    Log(EchoVR::LogLevel::Info,
        "[NEVR.HEADLESS] graphics settings apply skipped calls=%llu (headless: the renderer setters need a live renderer)",
        static_cast<unsigned long long>(calls));
  }
  return 0;
}

}  // namespace

bool Install(std::uintptr_t gameBase) {
  if (g_installed) return true;
  void* const target = nevr::ResolveVA_Checked(gameBase, nevr_render_config::kApplyVa);
  const auto& prologue = nevr_render_config::kApplyPrologue;
  if (!nevr::ValidatePrologue(target, prologue.data(), prologue.size())) {
    Log(EchoVR::LogLevel::Warning,
        "[NEVR.HEADLESS] graphics settings hook off name=ApplyRuntimeGfxSettings va=0x%llx reason=prologue_mismatch "
        "expected=%s actual=%s (this headless run calls the game's apply unhooked; its renderer setters need a GPU)",
        static_cast<unsigned long long>(nevr_render_config::kApplyVa), HexBytes(prologue.data(), 8).c_str(),
        HexBytes(target, 8).c_str());
    return false;
  }

  g_original = reinterpret_cast<ApplyFn>(target);
  const BOOL hooked = PatchDetour(&g_original, reinterpret_cast<PVOID>(&ApplyHook), "ApplyRuntimeGfxSettings");
  Log(hooked ? EchoVR::LogLevel::Info : EchoVR::LogLevel::Warning,
      "[NEVR.HEADLESS] graphics settings hook %s name=ApplyRuntimeGfxSettings va=0x%llx (the apply is skipped on headless)",
      hooked ? "installed" : "failed", static_cast<unsigned long long>(nevr_render_config::kApplyVa));
  g_installed = hooked != FALSE;
  return g_installed;
}

#ifdef NEVR_HEADLESS_GFX_TESTING
void ResetForTest() {
  if (g_installed) Hooking::Detach(reinterpret_cast<PVOID*>(&g_original), reinterpret_cast<PVOID>(&ApplyHook));
  g_original = nullptr;
  g_installed = false;
  g_calls.store(0, std::memory_order_relaxed);
}

bool ActiveForTest() { return g_installed; }
#endif

}  // namespace nevr_headless_gfx_settings
