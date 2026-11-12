/* The refresh-rate rule (#2172, #2000, #1648, docs/adr/0046 rule 3): 90 Hz (72 Hz on a Quest 2) while a combat map (or, with
 * `refresh_rate_lobby`, the combat lobby) is loaded, the game's own rate (120 on a Quest) in an Echo Arena level, in every other lobby and
 * everywhere else.
 *
 * Owner (ADR 0039). This file owns the rate `vrapi_SetDisplayRefreshRate` is called with. It is the one decision about it: the
 * render-configuration hook (sentinel/render_config_hook.cpp) holds the slot and asks `Choose` for the rate its original is called with;
 * no other module hooks that slot, and the q15t plugin no longer does. The inputs it combines, each named on every line it writes:
 *   - the game's request (`asked_hz`), and the session rate it keeps from it;
 *   - the `combat:` keys `refresh_rate` (the rule; off: the game's rate always), `refresh_rate_lobby` (the combat lobby, `mpl_lobby_b_combat`,
 *     also asks for the level rate; off: combat maps only) and `refresh_rate_quest2_72` (a Quest 2 asks 72; off: it asks 90 too);
 *     `refresh_rate` and `refresh_rate_quest2_72` are on by default, `refresh_rate_lobby` is OFF (combat_install.h, quest_config.h CombatConfig);
 *   - the headset: a Quest 2 or not (`quest2`), from the profile the identity step chose (quest/headset_active.h, ADR 0031 decision 4);
 *     this file reads no system property. With no profile set the generic one is taken and one Warning says so;
 *   - the loaded levels (`level`, the deciding level's CSymbol64, from combat_level.h): a combat map or (with refresh_rate_lobby) the combat lobby asks for the level rate, an
 *     Echo Arena level keeps the session rate and wins over any other level still loaded during a transition.
 *
 * Arena (same APK). Arena and combat run on the same APK, so the rule is scoped by the loaded level, not by the APK: in an Echo Arena match the
 * game's own rate stands, and a player who leaves combat for arena gets the session rate back at the tick's next look. The other lobbies are
 * neither: the lobby levels arena players use (`mpl_lobby_b`, `_b2`, ...) keep the game's rate always, and only the combat lobby can ask for the
 * level rate, and only with `refresh_rate_lobby` on. Where this departs from the q15t plugin (and docs/quest/combat/adr/0002): the plugin took
 * every lobby level and turned it on by default; the game holds 118-120 Hz in the lobby (ADR 0001), so arena's lobby is left alone.
 * The kill files `hzlevel.off`, `hzlobby90.off` and `hz72q2.off` of the plugin are no longer read: one found in the plugin's data directory is a
 * Warning (`combat_refresh_rate_legacy_switch`) naming the key that replaces it.
 *
 * Which APKs (ADR 0046 rule 5). The q15t and combat variants only: with `combat::Active()` false `Choose` answers the game's own rate,
 * `Note` writes nothing and no tick is registered, so the release APK, whose base carries the reference build's own combat modules,
 * has no second owner of the rate.
 *
 * How it works. `Choose(ovr, hz)` keeps the game's request as the session rate and the `ovr` pointer the game passed (the tick needs
 * it), and answers the level rate when the request is above it and a level of the rule is loaded. The tick (`Tick`, a combat_tick.h
 * callback, once per frame, looking every 0.5 s) re-asks on a change of level: it calls the function now in
 * `vrapi_SetDisplayRefreshRate`'s slot, so the render-configuration hook sees the call and its line reports the rate sent. The re-ask
 * is recognised by the thread that makes it (`Choose` answers it unchanged and `Note` skips it), so a re-ask is never taken for a
 * request of the game's. Nothing is asked while the session rate is at or below the level rate, or before the game's first request.
 *
 * Origin. The reference build's `hz` rule, reconstructed here in our own code (nothing verbatim): the level lists, CR15Game's two
 * gamespace tables (offsets 0x1f48/0x1f78 and 0x1f88/0x1fb8) and a gamespace's level at +0x58 are the pinned build's
 * (docs/quest/combat/adr/0001-match-refresh-rate.md, 0002-level-refresh-rate-default.md). The q15t plugin carried it as
 * `refresh_rate.cpp` (shipped as `hzlobby90` and `hz72q2`) until this module replaced it.
 *
 * Frequency (ADR 0038). `Choose` and `Note` run once per request of the game, which asks once per session start; the tick runs once per
 * frame and does one clock read and one compare, and looks at the game every 0.5 s (a few reads with process_vm_readv, which never
 * faults). Not measured on a headset.
 *
 * Lines (sentinel log):
 *   install  {"event":"combat_refresh_rate_install","mode":"witness|match|lobby","refresh_rate":<0|1>,"refresh_rate_lobby":<0|1>,
 *             "refresh_rate_quest2_72":<0|1>,"quest2":<0|1>,"level_hz":"<n>","tick":"registered|full|not_needed","msg":".."}
 *   request  {"event":"combat_refresh_rate_request","n":<n>,"asked_hz":..,"sent_hz":..,"rc":<n>,"why":"game request|kept at the level
 *             rate","mode":..,"quest2":..}   one per request of the game, after the original returned
 *   switch   {"event":"combat_refresh_rate_switch","n":<n>,"asked_hz":<session>,"sent_hz":..,"rc":<n>,"level":"%016llx",
 *             "why":"..","mode":..,"quest2":..}   one per change of level, after the re-ask returned
 *   missing  {"event":"combat_refresh_rate_profile_missing","profile":"generic","level_hz":"90","consequence":".."}   Warning, once, when
 *             the install step finds no headset profile set; `level_hz` only with the rule on (with it off the rule asks no level rate, the
 *             line has no `level_hz` and its consequence says so)
 *
 * Built with -fno-exceptions, like every translation unit that includes callback_thunk.h.
 */
#pragma once

#include <cstddef>
#include <cstdint>

#include "quest/combat/combat_install.h"
#include "quest/combat/combat_level.h"
#include "quest/sentinel/got_hook.h"

namespace nevr_quest::combat::refresh_rate {

inline constexpr float kLevelHz = 90.0f;
inline constexpr float kQuest2LevelHz = 72.0f;

// The rule's inputs besides the loaded levels, and what it makes of them (pure).
struct Inputs {
  bool rule = true;       // combat.refresh_rate
  bool lobby = false;     // combat.refresh_rate_lobby
  bool quest2At72 = true; // combat.refresh_rate_quest2_72
  bool quest2 = false;    // the headset is a Quest 2
};
struct Rule {
  bool on = false;
  bool lobby = false;
  float levelHz = kLevelHz;
};
Rule Decide(const Inputs& inputs) noexcept;

// The rate to send for a request of `askedHz`: `rule.levelHz` when the rule is on, a level of it is loaded (`inLevel`) and the request
// is above the level rate; `askedHz` otherwise (pure).
float RateFor(const Rule& rule, bool inLevel, float askedHz) noexcept;

// The rate the render-configuration handler calls vrapi_SetDisplayRefreshRate with. `ovr` is the pointer the game passed.
float Choose(void* ovr, float askedHz) noexcept;
// After the original returned: counts and writes the `combat_refresh_rate_request` line. A re-ask of the tick is not counted.
void Note(float askedHz, float sentHz, int result) noexcept;

// The module's normal step, with the three keys' values (combat_install.cpp reads them from the configuration): reads the identity step's
// headset profile (quest/headset_active.h), publishes the rule, writes the install line (and a Warning for each legacy kill file found)
// and registers the tick.
ModuleResult Install(bool rule, bool lobby, bool quest2At72) noexcept;

// The callback for combat_tick.h. Exposed for the host test.
void Tick(std::uintptr_t game) noexcept;

struct Stats {
  std::uint32_t requests;
  std::uint32_t switches;
  float lastAsked;
  float lastSent;
  bool inLevel;
};
Stats CurrentStats() noexcept;

// ---- test support ------------------------------------------------------------------------------------------------------

// The function the tick re-asks through (production: the function now in vrapi_SetDisplayRefreshRate's slot in libr15). nullptr
// restores production.
using AskFn = int (*)(void* ovr, float hz) noexcept;
void SetAskForTest(AskFn fn) noexcept;
// The directory the legacy kill files are looked for in (production: the data directory, legacy_switch.h legacy::Directory).
// nullptr or "" restores production.
void SetLegacyDirForTest(const char* dir) noexcept;
// How the re-ask finds libr15 (production: the loaded image). The re-ask reads vrapi_SetDisplayRefreshRate's slot in the image this finds
// each time it asks, so a test with a word of its own in a block of memory drives the production path. nullptr restores production.
void SetImageLookupForTest(sentinel::ImageLookup lookup) noexcept;
// The monotonic clock in seconds (nullptr restores CLOCK_MONOTONIC).
void SetClockForTest(double (*clock)() noexcept) noexcept;
// Forgets the rule, the session, the counters and the tick's schedule.
void ResetForTest() noexcept;

}  // namespace nevr_quest::combat::refresh_rate
