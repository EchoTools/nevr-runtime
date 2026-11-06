#pragma once

// The game's error dialog (echovr.exe 0x1400EA730, nevr_patch_addresses::GAME_ERROR_DIALOG) logs the title and the
// message of the box that tells the player the game cannot start, at level 2 (info), before it shows the box (#2245).
// The hook marks the thread while the dialog runs, and the built-in filter records every line logged in that window
// at error (core/log_level.h, Effective), keeping the game's level as game_level (log/run_log_record.h).

#include <cstdint>

#include "runtime/log/error_dialog_depth.h"  // Inside()

namespace nevr_error_dialog {

/// Installs the hook through PatchDetour (HookGuard, N84) after checking the function's first 16 bytes. A mismatch or
/// a failed hook is a Warning with the address and the bytes (logging.md Rule 6); the game's lines then keep the
/// level the game gave them. Called once, from the built-in filter's Init, in the boot phase.
void Install(std::uintptr_t gameBase);

}  // namespace nevr_error_dialog
