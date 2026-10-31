#pragma once

#include <cstdint>

namespace nevr_noovr_arglookup {

/// Installs the -noovr waiver on CMainArgs::FindArgIndexBySymbol. Call once at boot, before the first
/// PreprocessCommandLine. When the callee prologue or the pinned -noovr call site differs, no detour is installed (a
/// Warning with the expected and actual bytes), and -noovr keeps the game's own -spectatorstream requirement.
void Install(std::uintptr_t gameBase);

}  // namespace nevr_noovr_arglookup
