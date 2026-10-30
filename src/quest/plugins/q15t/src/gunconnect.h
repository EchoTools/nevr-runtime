// Gun connect (q15t the source build's gunconnect, the Fix only): on Quest offline practice the local player's guns are never
// connected, so they do not fire. CR15NetGunCS::Connect(cs, local user id) connects every gun whose owner slot is that
// user's; online the lobby-member event does it within a frame, offline nothing does. This calls it from the game tick:
// for each gun component system, when (a) some local gun is unconnected (inst+0xd8 != 0), (b) the connect ring is
// empty and (c) the local member's actor carries the combat tag, and (a)-(c) held for 2 s, Connect(cs, userId) once;
// re-check after 1 s and retry while (a) holds, up to 10 times, then log and stop for that gun CS.
// Runs on the game-thread tick (game_tick.h), 4 Hz while something is unconnected, else 1 Hz.
// Calls into the game library only (fixed addresses, base + ELF vaddr, cross-checked against the exported names); no
// code is written, no word is changed. Off: <data dir>/gunconnect.off.
#pragma once

#include <cstdint>

namespace q15t {

struct GunGuid {  // SActorGUID: returned by value in x0/x1 (CR15NetGame::UserActor), passed by reference to HasTag
  std::uint64_t lo, hi;
};

struct GunConnectFns {
  void (*connect)(void* cs, std::uint32_t user_id);                              // CR15NetGunCS::Connect(SR15NetUserId)
  GunGuid (*user_actor)(void* net, std::uint16_t member_index);                  // CR15NetGame::UserActor(u16) const
  bool (*has_tag)(void* game, const GunGuid* guid, std::uint64_t tag);           // CTagCS::HasTag(CncaGameMT*, const SActorGUID&, CSymbol64)
  std::uintptr_t tag_combat_addr;                                                // &CR15ActorUtils::kActorTagCombat (a CSymbol64)
  int named;                                                                     // how many of the four were confirmed by their exported name (log only)
};

// The pinned build's four addresses (base + ELF vaddr). `lookup` is dlsym on the loaded game library (nullptr: no
// name lookup, fixed addresses only). A name that resolves to anything but its fixed address is a refusal (false).
bool ResolveGunConnectFns(std::uintptr_t base, void* (*lookup)(const char* symbol), GunConnectFns* out, const char** why);

// `base`: the game library's load base. An all-null `fns` means "resolve from the loaded libr15 on the first tick".
void InitGunConnect(std::uintptr_t base, const GunConnectFns& fns);

// A TickFn (game_tick.h).
void GunConnectTick(std::uintptr_t game);

// 0 not started, 1 running, 2 off (switch file, or the library could not be resolved)
int GunConnectPhase();

// Test seams: a clock, a data directory override.
void SetGunConnectClockForTest(double (*clock)());
void SetGunConnectDirForTest(const char* dir);
void ResetGunConnectForTest();

}  // namespace q15t
