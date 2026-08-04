#pragma once

#include <windows.h>

/// Replace PSN- provider prefix with DSC- across all three string tables
/// in the game binary. Reuses PSN's provider_id slot (Nakama enum value 1)
/// so all ~70+ inlined format switches automatically produce "DSC-".
VOID PatchDscProvider();

/// Detour CNSUser::GetProviderPrefix (fcn.14060d640, 14 callers) to always
/// return the OVR-ORG string-table entry.  This is the single choke-point
/// for every xpid the game constructs — CreateUser, SaveLocalData,
/// LobbyFindSession, LobbyPlayerSessions, and three Send() paths all flow
/// through this one function.  Hooking it makes every prefix consistent
/// without touching string tables or CNSUser state nibbles.
VOID PatchProviderPrefixOvrOrg();
