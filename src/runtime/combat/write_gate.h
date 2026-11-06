#pragma once

// The one check every write of the combat module passes before it touches the game's memory: a detour, a byte
// patch, a code cave, or a cell in the .data tail Cat's scripts share. It asks the runtime's own registry
// (nevr_hook_guard::FindOverlap: every detour HookGuard recorded and every range ProcessMemcpy wrote) whether the
// range overlaps anything of ours, and refuses the write when it does. Cat's tools once wrote over our hooks; a
// refused write leaves that feature off and says which two ranges met.

#include <cstddef>

namespace nevr_combat {

/// True when [start, start + length) overlaps nothing the runtime hooked or wrote, and the write may go ahead.
/// False refuses it: one Warning names the write (`what`), its range, and the runtime's range it met (owner name,
/// kind, start, length). The caller then leaves the feature off.
bool GateWrite(const void* start, std::size_t length, const char* what);

}  // namespace nevr_combat
