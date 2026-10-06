#pragma once

#include <windows.h>

/// Replace PSN- provider prefix with DSC- across all three string tables
/// in the game binary. Reuses PSN's provider code (2, the same value in the
/// game's and Nakama's numbering) so all ~70+ inlined format switches
/// automatically produce "DSC-".
VOID PatchDscProvider();

/// Detour CNSUser::GetProviderPrefix (fcn.14060d640, 14 callers) to always
/// return the OVR-ORG string-table entry.  It is one of two xpid choke-points:
/// CreateUser, SaveLocalData, LobbyFindSession, LobbyPlayerSessions and three
/// Send() paths flow through it.  The other, GetUserIDString (echovr.exe
/// 0x1401ba630, 40 callers), has its own switch over the same string table
/// and is not hooked; see PatchAddresses::GET_USER_ID_STRING.
VOID PatchProviderPrefixOvrOrg();
