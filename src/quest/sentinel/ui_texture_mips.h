// Splash, loading and menu textures at their top mip on Quest (#961). The PC side is
// runtime/patch/ui_texture_mips.cpp; the decisions (which names are UI, when a texture still needs forcing) are shared
// through runtime/patch/ui_texture_mips_rules.h.
#pragma once

#include <cstddef>

namespace nevr_quest::ui_texture_mips {

// Installs the inline detour on CGTextureResource::Attach in the pinned libr15.so, and checks the pinned
// ForceLoadAllMips the detour calls. Returns the number of hooks installed (0 or 1). Install must run before the game
// attaches its first texture; a refusal is logged with its cause and changes nothing.
std::size_t InstallUiTextureMips() noexcept;

}  // namespace nevr_quest::ui_texture_mips
