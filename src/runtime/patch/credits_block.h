#pragma once

// #938: the project section of the in-game CREDITS page, rendered at build time from config/credits.json by
// tools/render_credits.py --ingame-source (src/runtime/CMakeLists.txt runs it) and compiled into the runtime.
// LF-terminated lines in the game's single-byte text; empty when no heading has a name. Defined in the
// generated credits_block.cpp, which holds no text of Ready At Dawn's.

#include <cstddef>

namespace nevr_credits_block {

extern const char kText[];
extern const std::size_t kSize;  // bytes in kText, the terminating NUL not counted

}  // namespace nevr_credits_block
