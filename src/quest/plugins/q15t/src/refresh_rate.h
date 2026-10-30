// The refresh-rate witness and the match-90 arm: a GOT wrapper on the game's import of vrapi_SetDisplayRefreshRate, and,
// in the match-90 arm, a game-thread tick that asks the runtime for 90 Hz while a combat map is loaded and for the game's
// own rate (120 on a Quest) everywhere else. q15t's source-build hz "hzmatch90" behaviour.
//
// Witness (both arms): every game request is logged with the rate sent. Match-90 (Q15T_HZ_MATCH90=1): a game request
// above 90 while a combat map is loaded is sent as 90; the tick re-asks on every change of "a combat map is loaded",
// looked at every 0.5 s; nothing is asked while the game's own rate is 90 or below, or before the game's first request
// (the tick needs the ovr pointer the game passed).
#pragma once

#include <cstdint>

#include "import_hook.h"

namespace q15t {

InstallResult InstallRefreshRate(std::uintptr_t base, std::uintptr_t slot_vaddr);

// The match-90 tick (register with AddGameTick). Exposed for the host test.
void RefreshMatchTick(std::uintptr_t game);
// The decision alone: true when one of the game's gamespaces holds a combat map; `level` gets the deciding level.
bool CombatMapLoaded(std::uintptr_t game, std::uint64_t* level);

struct HzStats {
  std::uint32_t calls, switches;
  float last_asked, last_sent;
  bool in_match;
};
HzStats GetHzStats();

// For the host test: forget the tick's 0.5 s schedule so the next tick looks at once.
void ResetHzScheduleForTest();

}  // namespace q15t
