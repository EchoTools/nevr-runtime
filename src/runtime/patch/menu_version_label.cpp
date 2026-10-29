#include "runtime/patch/menu_version_label.h"

#include <windows.h>

#include <array>
#include <atomic>
#include <string>

#include "core/build_identity.h"
#include "core/logging.h"
#include "nevr/pc/nevr_common.h"
#include "runtime/hook/hook_guard.h"
#include "runtime/hook/patching.h"
#include "runtime/log/boot_log_tee.h"
#include "runtime/patch/canvas_set_text.h"
#include "runtime/patch/menu_version_label_rules.h"

namespace nevr_menu_version_label {
namespace {

constexpr std::uint64_t kSetTextVA = nevr_canvas_set_text::kSetTextVA;
constexpr const auto& kSetTextPrologue = nevr_canvas_set_text::kSetTextPrologue;
using SetTextFn = nevr_canvas_set_text::SetTextFn;

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
