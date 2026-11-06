#include "runtime/log/error_dialog_level.h"

#include <windows.h>

#include <array>
#include <cstdint>
#include <cstdio>
#include <string>

#include "core/logging.h"
#include "nevr/pc/nevr_common.h"
#include "runtime/hook/addresses.h"
#include "runtime/hook/patching.h"
#include "runtime/log/error_dialog_depth.h"

namespace nevr_error_dialog {
namespace {

// echovr.exe 0x1400EA730: void(HWND owner, const char* message, const char* title). The message goes to MessageBoxW
// as its text and the title as its caption (the disassembly at 0x1400EA7AA on).
using ErrorDialogFn = void (*)(HWND owner, const char* message, const char* title);

constexpr std::array<std::uint8_t, 16> kPrologue = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C,
                                                    0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18, 0x57};

ErrorDialogFn g_original = nullptr;

void ErrorDialogHook(HWND owner, const char* message, const char* title) {
  Run([&] { g_original(owner, message, title); });
}

std::string Hex(const void* address, std::size_t count) {
  std::string out;
  char byte[4];
  for (std::size_t i = 0; i < count; ++i) {
    std::snprintf(byte, sizeof(byte), "%02x", static_cast<const std::uint8_t*>(address)[i]);
    out += byte;
  }
  return out;
}

}  // namespace

void Install(std::uintptr_t gameBase) {
  const unsigned long long va = 0x140000000ULL + nevr_patch_addresses::GAME_ERROR_DIALOG;
  g_original = reinterpret_cast<ErrorDialogFn>(gameBase + nevr_patch_addresses::GAME_ERROR_DIALOG);
  if (!nevr::ValidatePrologue(reinterpret_cast<const void*>(g_original), kPrologue.data(), kPrologue.size())) {
    Log(EchoVR::LogLevel::Warning,
        "[NEVR.LOGFILTER] hook failed name=GameErrorDialog va=0x%llX expected=%s actual=%s; the lines the game's error "
        "dialog logs keep the game's level (info) instead of error",
        va, Hex(kPrologue.data(), kPrologue.size()).c_str(), Hex(reinterpret_cast<const void*>(g_original), kPrologue.size()).c_str());
    g_original = nullptr;
    return;
  }
  // PatchDetour records the target with HookGuard and logs its own Warning when the hook fails (N84, N126).
  if (!PatchDetour(&g_original, reinterpret_cast<PVOID>(&ErrorDialogHook), "GameErrorDialog")) {
    g_original = nullptr;
    return;
  }
  Log(EchoVR::LogLevel::Info,
      "[NEVR.LOGFILTER] game error dialog hooked va=0x%llX; the title and message it logs are recorded at error", va);
}

}  // namespace nevr_error_dialog
