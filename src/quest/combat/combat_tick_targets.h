/* The slot of the game-thread tick (combat_tick.h): `CncaGame::ExclusiveUpdateGame`, which `CncaGame::Render` calls once per frame on the
 * game thread with x0 = the CR15Game. Pinned to the store libr15 (build id in pinned_targets.h, `LibR15PatchedBuildId`), the addresses
 * the q15t plugin's tick wrapper has used since it was written (`plugin.cpp`: Q15T_SLOT_EXCLUSIVEUPDATEGAME, Q15T_REAL_EXCLUSIVEUPDATEGAME,
 * Q15T_GAME_VPTR); not yet checked against the file by a pinned test (needs the store APK).
 */
#pragma once

#include <cstdint>

#include "quest/sentinel/callback_thunk.h"
#include "quest/sentinel/got_hook.h"
#include "quest/sentinel/pinned_targets.h"

namespace nevr_quest::combat::tick::targets {

inline constexpr const char* kSymbol = "_ZN10NRadEngine8CncaGame19ExclusiveUpdateGameEv";
inline constexpr std::uint64_t kSlotVaddr = 0x36e7f28ULL;      // the JUMP_SLOT libr15 calls it through
inline constexpr std::uint64_t kStockVaddr = 0x17ef62cULL;     // the game's function: what a slot nobody hooked holds
inline constexpr std::uint64_t kGameVptrVaddr = 0x362ed30ULL;  // the value CR15Game's first word holds

struct Tag {};
using Sig = void(void* game);
using Thunk = sentinel::CallbackThunk<Tag, Sig>;

// No expected original: the slot holds the game's function, or the wrapper another module put there first (the q15t plugin's tick
// wrapper, which installs before the combat normal step), and the manager takes either as the function this hook calls (got_hook.h, "the
// first handler on a slot"). A word that is no executable address is refused.
inline sentinel::GotTarget Target() {
  return sentinel::GotTarget{sentinel::pinned::kLibR15, kSymbol, sentinel::RelocKind::kJumpSlot,
                             sentinel::pinned::LibR15PatchedBuildId(), kSlotVaddr}
      .Owned("combat_tick");
}

}  // namespace nevr_quest::combat::tick::targets
