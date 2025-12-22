/**
 * @file frame_processor.cpp
 * @brief Implementation of frame processing and event detection
 */

#include "frame_processor.h"

#include <google/protobuf/util/json_util.h>

#include <chrono>

// Include generated protobuf headers
#include "apigame/http_v1.pb.h"
#include "telemetry/v1/telemetry.pb.h"

namespace TelemetryAgent {

FrameProcessor::FrameProcessor() : m_frameIndex(0) {}

FrameProcessor::~FrameProcessor() = default;

bool FrameProcessor::ProcessFrame(const FrameData& data, telemetry::v1::LobbySessionStateFrame& frame) {
  if (!data.session.valid) {
    return false;
  }

  // Parse session JSON
  enginehttp::SessionResponse session;
  if (!ParseSessionJson(data.session.json, session)) {
    return false;
  }

  // Store session UUID
  if (!session.session_id().empty()) {
    m_sessionUUID = session.session_id();
  }

  // Set frame metadata
  frame.set_frame_index(m_frameIndex++);

  // Set timestamp
  auto* timestamp = frame.mutable_timestamp();
  auto now = std::chrono::system_clock::now();
  auto seconds = std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch());
  auto nanos = std::chrono::duration_cast<std::chrono::nanoseconds>(now.time_since_epoch()) -
               std::chrono::duration_cast<std::chrono::nanoseconds>(seconds);
  timestamp->set_seconds(seconds.count());
  timestamp->set_nanos(static_cast<int32_t>(nanos.count()));

  // Copy session data to frame
  *frame.mutable_session() = session;

  // Parse and set player bones if available
  if (data.playerBones.valid) {
    enginehttp::PlayerBonesResponse bones;
    if (ParsePlayerBonesJson(data.playerBones.json, bones)) {
      *frame.mutable_player_bones() = bones;
    }
  }

  // Detect events by comparing to previous frame
  DetectEvents(session, frame);

  // Store current frame for next comparison
  m_previousSession = std::make_unique<enginehttp::SessionResponse>(session);

  return true;
}

void FrameProcessor::Reset() {
  m_previousSession.reset();
  m_sessionUUID.clear();
  m_frameIndex = 0;
}

std::string FrameProcessor::GetSessionUUID() const { return m_sessionUUID; }

bool FrameProcessor::ParseSessionJson(const std::string& json, enginehttp::SessionResponse& session) {
  google::protobuf::util::JsonParseOptions options;
  options.ignore_unknown_fields = true;

  auto status = google::protobuf::util::JsonStringToMessage(json, &session, options);
  return status.ok();
}

bool FrameProcessor::ParsePlayerBonesJson(const std::string& json, enginehttp::PlayerBonesResponse& bones) {
  google::protobuf::util::JsonParseOptions options;
  options.ignore_unknown_fields = true;

  auto status = google::protobuf::util::JsonStringToMessage(json, &bones, options);
  return status.ok();
}

void FrameProcessor::DetectEvents(const enginehttp::SessionResponse& current,
                                  telemetry::v1::LobbySessionStateFrame& frame) {
  if (!m_previousSession) {
    // First frame - detect initial player joins
    for (const auto& team : current.teams()) {
      for (const auto& player : team.players()) {
        auto* event = frame.add_events();
        auto* playerJoined = event->mutable_player_joined();
        *playerJoined->mutable_player() = player;

        // Determine role based on team name
        if (team.team_name() == "BLUE TEAM") {
          playerJoined->set_role(telemetry::v1::ROLE_BLUE_TEAM);
        } else if (team.team_name() == "ORANGE TEAM") {
          playerJoined->set_role(telemetry::v1::ROLE_ORANGE_TEAM);
        } else {
          playerJoined->set_role(telemetry::v1::ROLE_SPECTATOR);
        }
      }
    }
    return;
  }

  const auto& prev = *m_previousSession;

  // Detect score changes
  if (current.blue_points() != prev.blue_points() || current.orange_points() != prev.orange_points() ||
      current.blue_round_score() != prev.blue_round_score() ||
      current.orange_round_score() != prev.orange_round_score()) {
    auto* event = frame.add_events();
    auto* scoreUpdate = event->mutable_scoreboard_updated();
    scoreUpdate->set_blue_points(current.blue_points());
    scoreUpdate->set_orange_points(current.orange_points());
    scoreUpdate->set_blue_round_score(current.blue_round_score());
    scoreUpdate->set_orange_round_score(current.orange_round_score());
    scoreUpdate->set_game_clock_display(current.game_clock_display());
  }

  // Detect goal scored (based on last_score change)
  if (current.has_last_score() && prev.has_last_score()) {
    const auto& currScore = current.last_score();
    const auto& prevScore = prev.last_score();

    // Check if the scorer changed (indicates new goal)
    if (currScore.person_scored() != prevScore.person_scored() || currScore.disc_speed() != prevScore.disc_speed()) {
      auto* event = frame.add_events();
      auto* goalScored = event->mutable_goal_scored();
      *goalScored->mutable_score_details() = currScore;
    }
  }

  // Detect game status changes (round start/end, pause, etc.)
  if (current.game_status() != prev.game_status()) {
    // Could add more specific event types here
  }

  // Detect possession changes
  auto getPossessor = [](const enginehttp::SessionResponse& s) -> int32_t {
    for (const auto& team : s.teams()) {
      for (const auto& player : team.players()) {
        if (player.has_possession()) {
          return player.slot_number();
        }
      }
    }
    return -1;
  };

  int32_t currPossessor = getPossessor(current);
  int32_t prevPossessor = getPossessor(prev);

  if (currPossessor != prevPossessor) {
    auto* event = frame.add_events();
    auto* possChange = event->mutable_disc_possession_changed();
    possChange->set_player_slot(currPossessor);
    possChange->set_previous_player_slot(prevPossessor);
  }

  // Detect stat changes for each player
  auto findPlayer = [](const enginehttp::SessionResponse& s, int32_t slot) -> const enginehttp::TeamMember* {
    for (const auto& team : s.teams()) {
      for (const auto& player : team.players()) {
        if (player.slot_number() == slot) {
          return &player;
        }
      }
    }
    return nullptr;
  };

  for (const auto& team : current.teams()) {
    for (const auto& player : team.players()) {
      const auto* prevPlayer = findPlayer(prev, player.slot_number());
      if (!prevPlayer) continue;

      const auto& currStats = player.stats();
      const auto& prevStats = prevPlayer->stats();

      // Check for stat increases
      if (currStats.stuns() > prevStats.stuns()) {
        auto* event = frame.add_events();
        auto* stun = event->mutable_player_stun();
        stun->set_player_slot(player.slot_number());
        stun->set_total_stuns(currStats.stuns());
      }

      if (currStats.saves() > prevStats.saves()) {
        auto* event = frame.add_events();
        auto* save = event->mutable_player_save();
        save->set_player_slot(player.slot_number());
        save->set_total_saves(currStats.saves());
      }

      if (currStats.passes() > prevStats.passes()) {
        auto* event = frame.add_events();
        auto* pass = event->mutable_player_pass();
        pass->set_player_slot(player.slot_number());
        pass->set_total_passes(currStats.passes());
      }

      if (currStats.steals() > prevStats.steals()) {
        auto* event = frame.add_events();
        auto* steal = event->mutable_player_steal();
        steal->set_player_slot(player.slot_number());
        steal->set_total_steals(currStats.steals());
      }

      if (currStats.blocks() > prevStats.blocks()) {
        auto* event = frame.add_events();
        auto* block = event->mutable_player_block();
        block->set_player_slot(player.slot_number());
        block->set_total_blocks(currStats.blocks());
      }

      if (currStats.interceptions() > prevStats.interceptions()) {
        auto* event = frame.add_events();
        auto* intercept = event->mutable_player_interception();
        intercept->set_player_slot(player.slot_number());
        intercept->set_total_interceptions(currStats.interceptions());
      }

      if (currStats.assists() > prevStats.assists()) {
        auto* event = frame.add_events();
        auto* assist = event->mutable_player_assist();
        assist->set_player_slot(player.slot_number());
        assist->set_total_assists(currStats.assists());
      }

      if (currStats.shots_taken() > prevStats.shots_taken()) {
        auto* event = frame.add_events();
        auto* shot = event->mutable_player_shot_taken();
        shot->set_player_slot(player.slot_number());
        shot->set_total_shots(currStats.shots_taken());
      }
    }
  }
}

}  // namespace TelemetryAgent
