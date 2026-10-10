#pragma once
// The co-op AI diagnostic's decisions (coop_ai_trace.cpp, issue #63), apart from the hooks that feed
// them, so they are unit-tested without the game.
//
// In echo_arena_public_ai the bots are added but never move. The bot blackboard's update,
// CR15AIBBNetbot vslot 4 (echovr.exe 0x1412c1a30), has two gates before any movement:
//   1. Setup. While BB+0x5f4 is set it looks up the goals (0x1412c10f0) and the nav waypoints
//      (0x1412c13d0) on the actor the lobby names (0x140181d80: netGame+0x40, vtable +0x40) and stores
//      the result in BB+0x5f8; 0 skips the rest of the update. An actor id of -1 fails both lookups
//      without a log line of the game's own.
//   2. Phase. 0x1412c2100 maps the match game_state to the bot phase BB+0x12a0:
//      pre_match_wait_for_ready_update -> 0, pre_launch_update (or main_loop_update from 0) -> 1,
//      main_loop_update from 2 -> 3, point_score/round_over/sudden-death states from 4 -> 5,
//      post_match_celebration_update -> 6.
// One line per bot when any of these changes, and a heartbeat with the game clock, names the gate.

#include <array>
#include <cstddef>
#include <cstdint>

namespace nevr_coop_ai_trace {

constexpr std::int32_t kNotRun = -1;          // goals/waypoints: the setup has not run for this bot
constexpr std::int64_t kActorNotRead = -2;    // lookupActor: never read (the lookup was not hooked)

// What one bot's blackboard showed after its update.
struct BotState {
  std::int32_t setupPending = 0;              // BB+0x5f4
  std::int32_t setupOk = 0;                   // BB+0x5f8
  std::int32_t goalsOk = kNotRun;             // 0x1412c10f0's last result
  std::int32_t waypointsOk = kNotRun;         // 0x1412c13d0's last result
  std::int64_t lookupActor = kActorNotRead;   // 0x140181d80 on the net game; -1: the lobby names none
  std::int32_t phase = 0;                     // BB+0x12a0
};

inline bool SameState(const BotState& a, const BotState& b) {
  return a.setupPending == b.setupPending && a.setupOk == b.setupOk && a.goalsOk == b.goalsOk &&
         a.waypointsOk == b.waypointsOk && a.lookupActor == b.lookupActor && a.phase == b.phase;
}

inline bool HeartbeatDue(std::uint64_t lastMs, std::uint64_t nowMs, std::uint64_t intervalMs) {
  return nowMs - lastMs >= intervalMs;
}

// What the trace keeps per bot between updates.
struct BotRecord {
  const void* bot = nullptr;  // the blackboard; nullptr = free slot
  BotState state;             // the last state the hooks saw (goals/waypoints/actor land here first)
  BotState logged;            // the last state written to the log
  bool everLogged = false;
  std::uint64_t lastLogMs = 0;
  std::int64_t lastLogClock = -1;
};

// A fixed table of bots, so the per-tick path never allocates. An arena match has at most 8 players.
template <std::size_t N>
class BotTable {
 public:
  // The bot's record, claiming a free slot for a new bot; nullptr when the table is full.
  BotRecord* Find(const void* bot) {
    BotRecord* free = nullptr;
    for (BotRecord& r : records_) {
      if (r.bot == bot) return &r;
      if (r.bot == nullptr && free == nullptr) free = &r;
    }
    if (free != nullptr) {
      *free = BotRecord{};
      free->bot = bot;
    }
    return free;
  }

 private:
  std::array<BotRecord, N> records_{};
};

}  // namespace nevr_coop_ai_trace
