// The events a telemetry frame carries that are found by comparing the frame's snapshot with the one before it (status change, score,
// goal, join, leave, team switch, stat counters). Pure over two snapshots, so a host test drives it (TelemetryStreamer::DetectEvents calls it).
#pragma once

#include <cstdint>

#include "runtime/server/telemetry_game_status.h"
#include "runtime/server/telemetry_snapshot.h"
#include "telemetry/v2/echo_arena.pb.h"

namespace nevr_telemetry {

inline void DetectEvents(const TelemetrySnapshot& curr, const TelemetrySnapshot& prev, telemetry::v2::EchoArenaFrame* frame) {
  // Detect game status change
  int32_t currStatus = static_cast<int32_t>(GameStatusFromSymbol(curr.gameStatusHash));
  int32_t prevStatus = static_cast<int32_t>(GameStatusFromSymbol(prev.gameStatusHash));
  if (currStatus != prevStatus && prevStatus != 0) {
    // Status transitions → specific events
    if (currStatus == static_cast<int32_t>(telemetry::v2::GAME_STATUS_ROUND_START)) {
      auto* ev = frame->add_events();
      ev->mutable_round_started();
    } else if (currStatus == static_cast<int32_t>(telemetry::v2::GAME_STATUS_ROUND_OVER)) {
      auto* ev = frame->add_events();
      ev->mutable_round_ended();
    } else if (currStatus == static_cast<int32_t>(telemetry::v2::GAME_STATUS_POST_MATCH)) {
      auto* ev = frame->add_events();
      ev->mutable_match_ended();
    }
  }

  // Detect score changes
  if (curr.bluePoints != prev.bluePoints || curr.orangePoints != prev.orangePoints) {
    auto* ev = frame->add_events();
    auto* sb = ev->mutable_scoreboard_updated();
    sb->set_blue_points(curr.bluePoints);
    sb->set_orange_points(curr.orangePoints);
  }

  // Detect goal scored (last score data changed)
  if (curr.lastScore.scorerPacked != prev.lastScore.scorerPacked &&
      curr.lastScore.pointAmount > 0) {
    auto* ev = frame->add_events();
    auto* gs = ev->mutable_goal_scored();
    gs->set_team(static_cast<telemetry::v2::Role>(curr.lastScore.team));
    gs->set_point_amount(curr.lastScore.pointAmount);
    gs->set_disc_speed(curr.lastScore.discSpeed);
    gs->set_distance_thrown(curr.lastScore.distanceThrown);
    gs->set_scorer_slot(curr.lastScore.scorerPacked & 0x1F);
    if ((curr.lastScore.assistPacked & 0x1F) != 0x1F) {
      gs->set_assist_slot(curr.lastScore.assistPacked & 0x1F);
    }
  }

  // Detect player join/leave and stat changes
  for (uint16_t i = 0; i < 16; i++) {
    bool currPresent = (i < curr.playerCount && curr.playerInfo[i].accountId != 0);
    bool prevPresent = (i < prev.playerCount && prev.playerInfo[i].accountId != 0);

    if (currPresent && !prevPresent) {
      auto* ev = frame->add_events();
      auto* pj = ev->mutable_player_joined();
      pj->set_slot(i);
      pj->set_account_number(curr.playerInfo[i].accountId);
      pj->set_display_name(curr.playerInfo[i].displayName);
    } else if (!currPresent && prevPresent) {
      auto* ev = frame->add_events();
      auto* pl = ev->mutable_player_left();
      pl->set_player_slot(i);
    } else if (currPresent && prevPresent) {
      // Detect team switch
      if (curr.playerInfo[i].teamIndex != prev.playerInfo[i].teamIndex) {
        auto* ev = frame->add_events();
        auto* sw = ev->mutable_player_switched_team();
        sw->set_player_slot(i);
        sw->set_new_role(static_cast<telemetry::v2::Role>(curr.playerInfo[i].teamIndex));
      }

      // Detect stat changes (stuns, blocks, saves, etc.)
      const auto& cs = curr.stats[i];
      const auto& ps = prev.stats[i];

      if (cs.stuns > ps.stuns) {
        auto* ev = frame->add_events();
        auto* stun = ev->mutable_player_stun();
        stun->set_player_slot(i);
        stun->set_total_stuns(cs.stuns);
      }
      if (cs.blocks > ps.blocks) {
        auto* ev = frame->add_events();
        auto* block = ev->mutable_player_block();
        block->set_player_slot(i);
        block->set_total_blocks(cs.blocks);
      }
      if (cs.saves > ps.saves) {
        auto* ev = frame->add_events();
        auto* save = ev->mutable_player_save();
        save->set_player_slot(i);
        save->set_total_saves(cs.saves);
      }
      if (cs.interceptions > ps.interceptions) {
        auto* ev = frame->add_events();
        auto* interception = ev->mutable_player_interception();
        interception->set_player_slot(i);
      }
      if (cs.steals > ps.steals) {
        auto* ev = frame->add_events();
        auto* steal = ev->mutable_player_steal();
        steal->set_player_slot(i);
      }
      if (cs.passes > ps.passes) {
        auto* ev = frame->add_events();
        auto* pass = ev->mutable_player_pass();
        pass->set_player_slot(i);
      }
      if (cs.shotsTaken > ps.shotsTaken) {
        auto* ev = frame->add_events();
        auto* shot = ev->mutable_player_shot_taken();
        shot->set_player_slot(i);
      }
    }
  }
}

}  // namespace nevr_telemetry
