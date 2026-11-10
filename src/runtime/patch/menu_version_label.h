#pragma once
// The runtime version on the menu (#747). The stock menu draws a versions panel (script ui_mnu_versions) whose
// lobby_version text item goes through CCanvasUICS::SetText; this hooks that call and draws
// nevr_build_identity::MenuVersionLabel() in place of the lobby build time. Decisions: menu_version_label_rules.h.

#include <cstdint>

namespace nevr_menu_version_label {

// Hooks CCanvasUICS::SetText (echovr.exe 0x14037d470) after validating its bytes; a mismatch is logged and the hook is
// skipped. Call after the hooking engine is up; logs through the boot tee (DllMain time).
void Install(std::uintptr_t gameBase);

// A filter on every SetText call, ahead of the version label: it returns the text the call draws (the text it was given to leave
// it). The combat module's PAUSE label is the one user (runtime/combat): one owner per function, so it rides this hook instead of
// detouring SetText a second time. Returns the game's SetText behind the hook, for a caller that draws text itself, or null when
// the hook is not installed (the filter is then not set).
using TextFilter = const char* (*)(void* canvas, std::uint32_t handle, std::uint64_t item, const char* text);
using SetTextFn = std::uint64_t (*)(void* canvas, std::uint32_t handle, std::uint64_t item, const char* text);
SetTextFn SetTextFilter(TextFilter filter);

}  // namespace nevr_menu_version_label
