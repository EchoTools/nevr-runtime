// Splash, loading and menu textures at their top mip on Quest (#961). The PC side is
// runtime/patch/ui_texture_mips.cpp; the decisions (which names are UI, when a texture still needs forcing) are shared
// through runtime/patch/ui_texture_mips_rules.h.
#pragma once

#include <cstddef>

#include "got_hook.h"

namespace nevr_quest::ui_texture_mips {

// Activates the table row `ui_texture.attach` (CGTextureResource::Attach in the patched libr15.so), and checks the pinned
// ForceLoadAllMips the detour calls. Returns the number of hooks installed (0 or 1). Install must run before the game
// attaches its first texture; a refusal is logged with its cause and changes nothing (the row stays pass-through).
// `lookup` finds the loaded libr15; a host test passes its own.
std::size_t InstallUiTextureMips(sentinel::ImageLookup lookup = sentinel::FindLoadedImage) noexcept;

}  // namespace nevr_quest::ui_texture_mips
