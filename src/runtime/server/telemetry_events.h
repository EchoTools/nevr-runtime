#pragma once

#include <cstdint>

#include "runtime/server/telemetry_game_status.h"
#include "runtime/server/telemetry_player_read.h"
#include "runtime/server/telemetry_snapshot.h"
#include "telemetry/v2/echo_arena.pb.h"

// The events a frame carries, found by comparing two snapshots (#2099). Pure: the streamer calls it on
// the telemetry thread with the snapshot it is sending and the one before, and a test calls it the same way.

// What event detection keeps across frames (telemetry thread only; reset when a session starts).
struct TelemetryEventState {
  // The last status that was not GAME_STATUS_UNSPECIFIED. The game's own state-to-string function
  // (echovr.exe 0x14016d510) has no name for the states between a score and the next round start, so a
  // frame between them carries GAME_STATUS_UNSPECIFIED; a transition is judged against this instead.
  telemetry::v2::GameStatus lastKnownStatus = telemetry::v2::GAME_STATUS_UNSPECIFIED;
};

// last_score.team as /session names it: 0 is blue, anything else orange (0x140155f43 `cmp word ptr
// [rax + 0x10], 0`).
inline telemetry::v2::Role RoleFromLastScoreTeam(int16_t team) {
  return team == 0 ? telemetry::v2::ROLE_BLUE_TEAM : telemetry::v2::ROLE_ORANGE_TEAM;
}

inline void DetectTelemetryEvents(const TelemetrySnapshot& curr, const TelemetrySnapshot& prev,
                                  TelemetryEventState& state, telemetry::v2::EchoArenaFrame* frame) {
  // A snapshot the game thread could not read holds nothing: compared with a read one it would make every
  // player leave or join and every score rise from 0. The streamer keeps the last read snapshot as `prev`.
  if (!curr.gameRead || !prev.gameRead) return;

  // Game status: a change from the last known status, which starts as the previous snapshot's. The first
  // status seen in a session sets it and sends nothing.
  if (state.lastKnownStatus == telemetry::v2::GAME_STATUS_UNSPECIFIED) {
    state.lastKnownStatus = GameStatusFromSymbol(prev.gameStatusHash);
  }
  const telemetry::v2::GameStatus currStatus = GameStatusFromSymbol(curr.gameStatusHash);
  if (currStatus != telemetry::v2::GAME_STATUS_UNSPECIFIED) {
    if (currStatus != state.lastKnownStatus && state.lastKnownStatus != telemetry::v2::GAME_STATUS_UNSPECIFIED) {
      if (currStatus == telemetry::v2::GAME_STATUS_ROUND_START) {
        frame->add_events()->mutable_round_started();
      } else if (currStatus == telemetry::v2::GAME_STATUS_ROUND_OVER) {
        frame->add_events()->mutable_round_ended();
      } else if (currStatus == telemetry::v2::GAME_STATUS_POST_MATCH) {
        frame->add_events()->mutable_match_ended();
      }
    }
    state.lastKnownStatus = currStatus;
  }

  // Score changes
  if (curr.bluePoints != prev.bluePoints || curr.orangePoints != prev.orangePoints) {
    auto* sb = frame->add_events()->mutable_scoreboard_updated();
    sb->set_blue_points(curr.bluePoints);
    sb->set_orange_points(curr.orangePoints);
  }

  // Goal scored: a team's score rose. The event takes its fields from the game's last-score record, which the
  // game writes at the goal; the record is no signal by itself, because its distance and assist fields keep
  // changing during play and its scorer stays the same when one player scores twice.
  if (curr.bluePoints > prev.bluePoints || curr.orangePoints > prev.orangePoints) {
    auto* gs = frame->add_events()->mutable_goal_scored();
    gs->set_team(RoleFromLastScoreTeam(curr.lastScore.team));
    gs->set_point_amount(curr.lastScore.pointAmount);
    gs->set_disc_speed(curr.lastScore.discSpeed);
    gs->set_distance_thrown(curr.lastScore.distanceThrown);
    gs->set_scorer_slot(curr.lastScore.scorerPacked & 0x1F);
    if ((curr.lastScore.assistPacked & 0x1F) != 0x1F) {
      gs->set_assist_slot(curr.lastScore.assistPacked & 0x1F);
    }
  }

  // Player join/leave, team switches and stat rises, each on the game slot. A slot that another account took
  // between the snapshots is a leave of the old player and a join of the new one, and their stats are not
  // compared. A stat that falls (a new match) sends nothing.
  for (uint8_t i = 0; i < nevr_telemetry_player::kMaxSlots; i++) {
    const auto& cp = curr.playerInfo[i];
    const auto& pp = prev.playerInfo[i];
    const bool samePlayer = cp.present && pp.present && cp.accountId == pp.accountId;

    if (pp.present && !samePlayer) {
      frame->add_events()->mutable_player_left()->set_player_slot(i);
    }
    if (cp.present && !samePlayer) {
      auto* pj = frame->add_events()->mutable_player_joined();
      pj->set_slot(i);
      pj->set_role(nevr_telemetry_player::RoleFromTeam(cp.teamIndex));
      pj->set_account_number(cp.accountId);
      pj->set_display_name(cp.displayName);
    }
    if (!samePlayer) continue;

    if (cp.teamIndex != pp.teamIndex) {
      auto* sw = frame->add_events()->mutable_player_switched_team();
      sw->set_player_slot(i);
      sw->set_new_role(nevr_telemetry_player::RoleFromTeam(cp.teamIndex));
      sw->set_prev_role(nevr_telemetry_player::RoleFromTeam(pp.teamIndex));
    }

    const auto& cs = curr.stats[i];
    const auto& ps = prev.stats[i];
    if (cs.stuns > ps.stuns) {
      auto* stun = frame->add_events()->mutable_player_stun();
      stun->set_player_slot(i);
      stun->set_total_stuns(cs.stuns);
    }
    if (cs.blocks > ps.blocks) {
      auto* block = frame->add_events()->mutable_player_block();
      block->set_player_slot(i);
      block->set_total_blocks(cs.blocks);
    }
    if (cs.saves > ps.saves) {
      auto* save = frame->add_events()->mutable_player_save();
      save->set_player_slot(i);
      save->set_total_saves(cs.saves);
    }
    if (cs.interceptions > ps.interceptions) {
      frame->add_events()->mutable_player_interception()->set_player_slot(i);
    }
    if (cs.steals > ps.steals) {
      frame->add_events()->mutable_player_steal()->set_player_slot(i);
    }
    if (cs.passes > ps.passes) {
      frame->add_events()->mutable_player_pass()->set_player_slot(i);
    }
    if (cs.shotsTaken > ps.shotsTaken) {
      frame->add_events()->mutable_player_shot_taken()->set_player_slot(i);
    }
  }
}
