/* The Quest 2 combat step-down's rules (#1737, docs/quest/combat/texture_quest2.md): when the texture-quality owner sends Very Low in
 * place of the level the player has, and when a Resize that change needs may be made.
 *
 * Pure: no game header, no hook, no clock of its own (every time is an argument, in CLOCK_MONOTONIC milliseconds), so the owner
 * (texture_menu_hook.cpp), the combat input (combat/texture_quest2.cpp) and their host tests share one definition.
 *
 * Owner and input (ADR 0039). texture_quality owns the eye size (texture_quality_hook.h); combat is an input that says where the game is
 * (texture_quality_input.h). With no input registered (the release APK, or `combat.texture_quest2_step_down` false) nothing here runs and the
 * level is the one the player has.
 *
 * The look (LookStep, run by the owner's poller twice a second). A look is what the combat input last saw: the class of the loaded levels
 * (`Where`) and whether any gamespace was still loading (the init-fiber table's count was not 0, or could not be read). The look turns ON when a
 * step-down place (a combat map, or the combat lobby with `combat.texture_quest2_lobby`) has been the place for kSettleMs and no gamespace has
 * been loading for kSettleMs. It turns OFF when an Echo Arena level has been the place for kSettleMs with no load for kSettleMs (`arena`), or
 * when no step-down place has been seen for kGapMs (`no_combat_level`): a lobby -> map -> lobby round trip keeps the look on through its load
 * gaps, so it adds no Resize there. A look older than kLookFreshMs says nothing: it is not quiet, and the gate keeps its state.
 *
 * The level (EffectiveLevel). Very Low only on a Quest 2, only while the player has not chosen a level (texture_quality::PlayerChose), only while
 * the look is on, and never after the look was turned off for the run (a Resize the engine refused, or the upload words could not be pinned).
 *
 * The Resize (ResizeHold). A change the look causes (stepping down, or back up when the look turns off) is made only when the input's look made
 * now is fresh, shows no gamespace loading, and no gamespace has loaded for kSettleMs (by the poller's quiet stretch and by the time of the last load
 * the input itself saw, whichever is stricter), and the game's upload count (0x379b400) reads 0 and its
 * upload lock (0x379b780) reads 0 (texture_quality_targets.h). The load-window rule is the guard; the count and lock read only narrows what
 * remains (docs/quest/combat/texture_quest2.md "What exposure remains"). A change the player makes in the menu keeps the menu's own path.
 */
#pragma once

#include <cstdint>

#include "texture_quality.h"

namespace nevr_quest::texture_quality::rules {

// Settle time after a level change or the end of a load (upstream's lead, kotorvr/echoquest-combat afk_texres.cpp, as a number only).
inline constexpr std::int64_t kSettleMs = 10000;
// How long the look stays on with no step-down place seen (a load gap, or a lobby the look does not cover).
inline constexpr std::int64_t kGapMs = 90000;
// A look older than this says nothing.
inline constexpr std::int64_t kLookFreshMs = 1500;
// The level a Quest 2 in combat is sent.
inline constexpr Level kStepDownLevel = Level::kVeryLow;

enum class Where : int { kNone = 0, kCombatMap = 1, kCombatLobby = 2, kEchoArena = 3 };

constexpr const char* WhereName(Where where) noexcept {
  switch (where) {
    case Where::kNone: return "none";
    case Where::kCombatMap: return "combat_map";
    case Where::kCombatLobby: return "combat_lobby";
    case Where::kEchoArena: return "echo_arena";
  }
  return "none";
}

// What the combat input saw at `atMs` (0: it has not looked yet).
struct Look {
  Where where = Where::kNone;
  bool loading = true;
  std::int64_t atMs = 0;
  // The time the input last saw a load (0: none yet). The input looks more often than the owner's poller reads, so a load that the next look
  // overwrote before the poller sampled is still in this word (ResizeHold).
  std::int64_t lastLoadMs = 0;
};

struct StepDownSwitches {
  bool lobby = true;  // combat.texture_quest2_lobby: the combat lobby is a step-down place too
};

enum class Place : int { kOther = 0, kStepDown = 1, kArena = 2 };

constexpr Place PlaceOf(Where where, StepDownSwitches switches) noexcept {
  if (where == Where::kCombatMap) return Place::kStepDown;
  if (where == Where::kCombatLobby) return switches.lobby ? Place::kStepDown : Place::kOther;
  if (where == Where::kEchoArena) return Place::kArena;
  return Place::kOther;
}

// Why the level sent is what it is (the `reason` field).
enum class RuleReason : int {
  kOff = 0,           // no input registered: the player's level
  kNotQuest2 = 1,     // not a Quest 2
  kPlayerChoice = 2,  // the player chose a level (config word, menu, reference rule, development property)
  kQuest2Combat = 3,  // the look is on: Very Low
  kNotInCombat = 4,   // the look has not turned on
  kArena = 5,         // the look turned off: an Echo Arena level settled
  kNoCombatLevel = 6, // the look turned off: no step-down place for kGapMs
  kLookOff = 7,       // the look was turned off for the run
};

constexpr const char* RuleReasonName(RuleReason reason) noexcept {
  switch (reason) {
    case RuleReason::kOff: return "off";
    case RuleReason::kNotQuest2: return "not_quest2";
    case RuleReason::kPlayerChoice: return "player_choice";
    case RuleReason::kQuest2Combat: return "quest2_combat";
    case RuleReason::kNotInCombat: return "not_in_combat";
    case RuleReason::kArena: return "arena";
    case RuleReason::kNoCombatLevel: return "no_combat_level";
    case RuleReason::kLookOff: return "look_off";
  }
  return "off";
}

struct LookGate {
  bool on = false;
  Place place = Place::kOther;
  std::int64_t placeSinceMs = 0;
  std::int64_t quietSinceMs = 0;  // 0: a load was seen in the last fresh look, or no fresh look yet
  RuleReason reason = RuleReason::kNotInCombat;
};

constexpr bool Fresh(const Look& look, std::int64_t nowMs) noexcept {
  return look.atMs != 0 && nowMs - look.atMs <= kLookFreshMs && nowMs >= look.atMs;
}

constexpr LookGate LookStep(LookGate gate, const Look& look, StepDownSwitches switches, std::int64_t nowMs) noexcept {
  if (!Fresh(look, nowMs)) {
    gate.quietSinceMs = 0;  // nothing is known about loads now
    return gate;
  }
  const Place place = PlaceOf(look.where, switches);
  if (place != gate.place) {
    gate.place = place;
    gate.placeSinceMs = nowMs;
  }
  if (look.loading) {
    gate.quietSinceMs = 0;
  } else if (gate.quietSinceMs == 0) {
    gate.quietSinceMs = nowMs;
  }
  const bool settled = nowMs - gate.placeSinceMs >= kSettleMs && gate.quietSinceMs != 0 && nowMs - gate.quietSinceMs >= kSettleMs;
  if (!gate.on) {
    if (place == Place::kStepDown && settled) {
      gate.on = true;
      gate.reason = RuleReason::kQuest2Combat;
    }
  } else if (place == Place::kArena && settled) {
    gate.on = false;
    gate.reason = RuleReason::kArena;
  } else if (place == Place::kOther && nowMs - gate.placeSinceMs >= kGapMs) {
    gate.on = false;
    gate.reason = RuleReason::kNoCombatLevel;
  }
  return gate;
}

struct StepDownInputs {
  Level requested = Level::kStock;  // the level the player has (the menu's request, else the applied one)
  bool registered = false;          // a combat input is registered (texture_quality_input.h)
  bool quest2 = false;              // the headset profile is the Quest 2's
  bool playerChose = false;         // texture_quality::PlayerChose()
  bool lookOffForRun = false;       // a look-driven Resize was refused, or the upload words are not pinned
  LookGate gate;
};

struct Effective {
  Level level;
  RuleReason reason;
};

constexpr Effective EffectiveLevel(const StepDownInputs& in) noexcept {
  if (!in.registered) return {in.requested, RuleReason::kOff};
  if (in.lookOffForRun) return {in.requested, RuleReason::kLookOff};
  if (!in.quest2) return {in.requested, RuleReason::kNotQuest2};
  if (in.playerChose) return {in.requested, RuleReason::kPlayerChoice};
  if (in.gate.on) return {kStepDownLevel, RuleReason::kQuest2Combat};
  return {in.requested, in.gate.reason};  // a gate that is off never holds kQuest2Combat (LookStep)
}

// Why a look-driven Resize is not made now (kNone: it may be made).
enum class Hold : int { kNone = 0, kStaleLook = 1, kLoading = 2, kSettling = 3, kUploads = 4, kLock = 5 };

constexpr const char* HoldName(Hold hold) noexcept {
  switch (hold) {
    case Hold::kNone: return "none";
    case Hold::kStaleLook: return "stale_look";
    case Hold::kLoading: return "loading";
    case Hold::kSettling: return "settling";
    case Hold::kUploads: return "uploads";
    case Hold::kLock: return "lock";
  }
  return "none";
}

struct ResizeInputs {
  Look look;                      // the input's look, read right before the Resize
  std::int64_t nowMs = 0;
  std::int64_t quietSinceMs = 0;  // the gate's (the poller's) start of the quiet stretch, 0 when not quiet
  std::uint64_t uploads = 1;      // the game's upload count
  std::uint32_t lock = 1;         // the game's upload lock
};

constexpr Hold ResizeHold(const ResizeInputs& in) noexcept {
  if (!Fresh(in.look, in.nowMs)) return Hold::kStaleLook;
  if (in.look.loading) return Hold::kLoading;
  if (in.quietSinceMs == 0 || in.nowMs - in.quietSinceMs < kSettleMs) return Hold::kSettling;
  if (in.look.lastLoadMs != 0 && in.nowMs - in.look.lastLoadMs < kSettleMs) return Hold::kSettling;  // a load the poller never sampled
  if (in.uploads != 0) return Hold::kUploads;
  if (in.lock != 0) return Hold::kLock;
  return Hold::kNone;
}

constexpr bool ResizeAllowed(const ResizeInputs& in) noexcept { return ResizeHold(in) == Hold::kNone; }

}  // namespace nevr_quest::texture_quality::rules
