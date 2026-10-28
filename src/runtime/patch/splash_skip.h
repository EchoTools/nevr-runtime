#pragma once
// A button press skips the splash screen (#962). Hooks the pointer-ray menu widget's per-frame update
// (CR15VRMenuWidgetCS, echovr.exe 0x140a7bdd0); when the widget holds the splash's Continue element and a click button
// goes down, it issues the click the on-screen button takes. Decisions: splash_skip_rules.h.

#include <cstdint>

namespace nevr_splash_skip {

// Validates the widget update, the click dispatch and the two button predicates by their bytes, then hooks the update.
// A mismatch is logged and nothing is installed. Call after the hooking engine is up; logs through the boot tee
// (DllMain time).
void Install(std::uintptr_t gameBase);

}  // namespace nevr_splash_skip
