#pragma once
// The runtime version on the menu (#747). The stock menu draws a versions panel (script ui_mnu_versions) whose
// lobby_version text item goes through CCanvasUICS::SetText; this hooks that call and draws
// nevr_build_identity::MenuVersionLabel() in place of the lobby build time. Decisions: menu_version_label_rules.h.

#include <cstdint>

namespace nevr_menu_version_label {

// Hooks CCanvasUICS::SetText (echovr.exe 0x14037d470) after validating its bytes; a mismatch is logged and the hook is
// skipped. Call after the hooking engine is up; logs through the boot tee (DllMain time).
void Install(std::uintptr_t gameBase);

}  // namespace nevr_menu_version_label
