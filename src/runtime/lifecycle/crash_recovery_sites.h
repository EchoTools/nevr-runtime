#pragma once

#include <array>
#include <cstdint>

namespace CrashRecovery {

struct KnownNullDerefSite {
  uint64_t rva;
  const char* name;
};

// Attribution-only table for the server-mode null-dereference family.  The VEH
// catches the entire fault class; this data makes its crash-safe output useful
// without adding a hook at every entry point.
// The original reconstruction contains 30 measured entries. Earlier planning
// material claimed 31; keeping the array exact prevents a silent null entry
// from reaching the crash handler.
inline constexpr std::array<KnownNullDerefSite, 30> kKnownNullDerefSites = {{
    {0xC540A0, "DispatchEvent[0]"},  {0xC54470, "DispatchEvent[1]"},
    {0xC54840, "DispatchEvent[2]"},  {0xC553B0, "DispatchEvent[3]"},
    {0xC55780, "DispatchEvent[4]"},  {0xC55B50, "DispatchEvent[5]"},
    {0xC55F20, "DispatchEvent[6]"},  {0xC562F0, "DispatchEvent[7]"},
    {0xC566C0, "DispatchEvent[8]"},  {0xC56A90, "DispatchEvent[9]"},
    {0xD09D60, "IsCompactPoolHandleValid_B"},
    {0xD098C0, "IsPunchableInCombatMode"},
    {0xD09E80, "GetPlayerBlockingState"},
    {0xD09CD0, "LookupPlayerWeaponHandle"},
    {0xD09DB0, "IsPlayerInUnassignedWeaponState"},
    {0xD09EB0, "IsPlayerInPunchState"},
    {0x12C2D0, "LoadoutBroadcast[0]"}, {0x130B00, "LoadoutBroadcast[1]"},
    {0x130E00, "LoadoutBroadcast[2]"}, {0x1A9B20, "LoadoutBroadcast[3]"},
    {0x1A9D60, "LoadoutBroadcast[4]"},
    {0x14E540, "~CR15NetLobby"},      {0x1B1910, "FindSpawnPoint"},
    {0x15F530, "OnMsgCurrentLoadoutRequest"},
    {0x1A79D0, "OnMsgSaveLoadoutRequest"},
    {0x1C98C0, "GetUserName"},        {0x113A90, "GetNetGameFromContext"},
    {0x170770, "GetHeadsetTypeName"}, {0x170730, "GetHeadsetTypeName_B"},
    {0x170750, "GetHeadsetTypeName_C"},
}};

inline const char* LookupKnownNullDerefSite(int64_t rva) {
  if (rva < 0) return nullptr;
  const uint64_t address = static_cast<uint64_t>(rva);
  const char* best = nullptr;
  uint64_t bestBase = 0;
  for (const KnownNullDerefSite& site : kKnownNullDerefSites) {
    if (address >= site.rva && address - site.rva < 0x800 && site.rva >= bestBase) {
      best = site.name;
      bestBase = site.rva;
    }
  }
  return best;
}

}  // namespace CrashRecovery
