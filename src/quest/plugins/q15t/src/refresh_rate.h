// The refresh-rate witness and the level rule: a GOT wrapper on the game's import of vrapi_SetDisplayRefreshRate (the game
// calls it once per session start, so every call is logged), and a game-thread tick (once per frame, looks every 0.5 s) that
// asks the runtime for the level rate while a combat map or the social lobby is loaded and for the game's own rate (120 on a
// Quest) in an Echo Arena level and everywhere else. q15t's shipped "hzlobby90" + "hz72q2" behaviour.
//
// The level rate is 90, or 72 on a Quest 2 (ro.product.device "hollywood"). Witness (always): every game request is logged
// with the rate sent (asked_hz, sent_hz). Rule (Q15T_HZ_MATCH90=1, the default): a game request above the level rate while
// such a level is loaded is sent at the level rate; the tick re-asks on every change; nothing is asked while the game's own
// rate is at or below the level rate, or before the game's first request (the tick needs the ovr pointer the game passed).
// Kill files in the plugin data directory (module_switch.h), read once at install: hzlevel.off (witness only), hzlobby90.off
// (combat maps only, q15t's hzmatch90 form), hz72q2.off (a Quest 2 uses 90 too).
#pragma once

#include <cstdint>

#include "import_hook.h"

namespace q15t {

InstallResult InstallRefreshRate(std::uintptr_t base, std::uintptr_t slot_vaddr);

// The match-90 tick (register with AddGameTick). Exposed for the host test.
void RefreshMatchTick(std::uintptr_t game);
// The decision alone: true when one of the game's gamespaces holds a combat map; `level` gets the deciding level.
bool CombatMapLoaded(std::uintptr_t game, std::uint64_t* level);

// What the loaded gamespaces decide: a combat map (or, with `lobby`, a social-lobby level) asks for the level rate; with
// `lobby`, an Echo Arena level wins and keeps the session rate. `level` gets the deciding level's CSymbol64, or 0.
enum class HzLevelKind { kNone, kCombatOrLobby, kEchoArena };
HzLevelKind ClassifyLoadedLevels(std::uintptr_t game, bool lobby, std::uint64_t* level);

// The rule this run uses, read from the compile option, the kill files and the device (logs q15t_refresh_rate_install).
struct HzRule {
  bool compiled, level_off, lobby_off, q2_off, quest2, on, lobby;
  float low_hz;
};
HzRule ConfigureHzRule();
// Test seam: the value of ro.product.device (null: the real property on Android, "" on a host).
void SetHzDeviceForTest(const char* device);

struct HzStats {
  std::uint32_t calls, switches;
  float last_asked, last_sent;
  bool in_match;
};
HzStats GetHzStats();

// For the host test: forget the tick's 0.5 s schedule so the next tick looks at once.
void ResetHzScheduleForTest();

}  // namespace q15t
