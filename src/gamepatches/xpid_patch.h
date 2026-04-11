#pragma once

#include <windows.h>

/// Replace PSN- provider prefix with DSC- across all three string tables
/// in the game binary. Reuses PSN's provider_id slot (Nakama enum value 1)
/// so all ~70+ inlined format switches automatically produce "DSC-".
VOID PatchDscProvider();
