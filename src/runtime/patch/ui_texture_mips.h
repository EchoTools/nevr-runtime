#pragma once
// UI, splash and menu textures at their top mip from the first frame (#961). The engine's texture streaming knows
// no UI class, so a UI texture can stay at its base level; this hooks CGTextureResource::Attach and, for a texture
// whose name is the UI system's (ui_texture_mips_rules.h), runs the game's own ForceLoadAllMips on it. World
// textures are not touched and keep the engine's streaming. Decisions: ui_texture_mips_rules.h.

#include <cstdint>

namespace nevr_ui_texture_mips {

// Hooks CGTextureResource::Attach (echovr.exe 0x1405491c0) after validating its bytes and those of
// ForceLoadAllMips (0x14056c420); a mismatch is logged and the hook is skipped. Call after the hooking engine is
// up; logs through the boot tee (DllMain time).
void Install(std::uintptr_t gameBase);

}  // namespace nevr_ui_texture_mips
