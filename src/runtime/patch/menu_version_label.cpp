#include "runtime/patch/menu_version_label.h"

#include <windows.h>

#include <array>
#include <atomic>
#include <string>

#include "core/build_identity.h"
#include "core/logging.h"
#include "nevr_common.h"
#include "runtime/hook/hook_guard.h"
#include "runtime/hook/patching.h"
#include "runtime/log/boot_log_tee.h"
#include "runtime/patch/menu_version_label_rules.h"

namespace nevr_menu_version_label {
namespace {

// CCanvasUICS::SetText(handle, CSymbol64 item, const char* text) (Quest 0x149111c in ReVault): (this = CCanvasUICS*,
// RDX = SCompactPoolHandle (u32), R8 = item symbol, R9 = NUL-terminated text) -> non-zero when the item exists. It
// returns 0 at once for a null text. Called by every SetText2 script node (0x14126dce0) and by the credits page.
// PUSH RBX; SUB RSP,0x40; MOV RBX,R9; TEST R9,R9; JZ; MOV R9,R8; LEA RAX,[RSP+0x30]; MOV R8D,EDX; MOV [RSP+0x20],RAX;
// LEA RDX,[RSP+0x68].
constexpr std::uint64_t kSetTextVA = 0x14037D470;
constexpr std::array<std::uint8_t, 32> kSetTextPrologue = {0x40, 0x53, 0x48, 0x83, 0xEC, 0x40, 0x49, 0x8B, 0xD9, 0x4D, 0x85,
                                                           0xC9, 0x74, 0x3F, 0x4D, 0x8B, 0xC8, 0x48, 0x8D, 0x44, 0x24, 0x30,
                                                           0x44, 0x8B, 0xC2, 0x48, 0x89, 0x44, 0x24, 0x20, 0x48, 0x8D};
using SetTextFn = std::uint64_t (*)(void* canvas, std::uint32_t handle, std::uint64_t item, const char* text);

SetTextFn g_originalSetText = nullptr;
std::string g_label;  // fitted, set once at Install and read-only afterwards
std::atomic<unsigned> g_replaced{0};

std::uint64_t SetTextHook(void* canvas, std::uint32_t handle, std::uint64_t item, const char* text) {
  // Every SetText call passes through here (the credits page sets text each frame), so the common case is one compare.
  if (item != kLobbyVersionItem) return g_originalSetText(canvas, handle, item, text);
  const char* replacement = ReplacementFor(item, text, g_label);
  if (replacement == nullptr) return g_originalSetText(canvas, handle, item, text);
  const std::uint64_t result = g_originalSetText(canvas, handle, item, replacement);
  Log(EchoVR::LogLevel::Info, "[NEVR.MENU] version label set item=lobby_version label=%s replaced_text=%s count=%u result=%llu",
      replacement, text, g_replaced.fetch_add(1, std::memory_order_relaxed) + 1, static_cast<unsigned long long>(result));
  return result;
}

}  // namespace

void Install(std::uintptr_t gameBase) {
  void* target = nevr::ResolveVA_Checked(gameBase, kSetTextVA);
  if (!nevr::ValidatePrologue(target, kSetTextPrologue.data(), kSetTextPrologue.size())) {
    nevr_boot_log_tee::TeeFprintf("[NEVR.MENU] CCanvasUICS::SetText hook skipped va=0x%llx reason=prologue_mismatch\n",
                                  static_cast<unsigned long long>(kSetTextVA));
    return;
  }
  const std::string full = nevr_build_identity::MenuVersionLabel();
  g_label = FitLabel(full);
  g_originalSetText = reinterpret_cast<SetTextFn>(target);
  if (PatchDetour(&g_originalSetText, reinterpret_cast<PVOID>(&SetTextHook), "CCanvasUICS::SetText")) {
    nevr_boot_log_tee::TeeFprintf("[NEVR.MENU] CCanvasUICS::SetText hook installed va=0x%llx label=%s full_label=%s\n",
                                  static_cast<unsigned long long>(kSetTextVA), g_label.c_str(), full.c_str());
  }
}

}  // namespace nevr_menu_version_label
