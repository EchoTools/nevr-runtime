#pragma once

#include <cstdint>
#include <string>

#include "runtime/server/telemetry_game_status.h"
#include "runtime/server/telemetry_player_read.h"
#include "runtime/server/telemetry_session_read.h"
#include "runtime/server/telemetry_snapshot.h"
#include "telemetry/v2/echo_arena.pb.h"

// The stream's messages built from a snapshot (#2100). Pure: the streamer calls these on the telemetry thread
// with the snapshot it sends, and a test calls them the same way.
namespace nevr_telemetry_frame {

// The arena part of the capture header: the session, the level and round count the game gives for it, the
// skeleton layout and the roster at the snapshot.
inline void BuildArenaHeader(const TelemetrySnapshot& snap, const std::string& sessionId,
                             telemetry::v2::EchoArenaHeader* ah) {
  ah->set_session_id(sessionId);
  ah->set_client_name("nevr-runtime");
  ah->set_map_name(snap.mapName);
  ah->set_total_round_count(snap.totalRoundCount);

  // Skeleton layout (fixed for EchoVR: 23 bones)
  auto* skel = ah->mutable_skeleton();
  skel->set_bone_count(23);
  skel->set_transform_stride(12);    // 3 x float32
  skel->set_orientation_stride(16);  // 4 x float32

  // Initial roster from snapshot
  for (uint8_t i = 0; i < nevr_telemetry_player::kMaxSlots; i++) {  // i is the game slot
    const auto& pi = snap.playerInfo[i];
    if (!pi.present) continue;
    auto* entry = ah->add_initial_roster();
    entry->set_slot(i);
    entry->set_account_number(pi.accountId);
    entry->set_display_name(pi.displayName);
    entry->set_role(nevr_telemetry_player::RoleFromTeam(pi.teamIndex));
    entry->set_jersey_number(pi.jerseyNumber);
    entry->set_level(static_cast<int32_t>(pi.level));
  }
}

// The game slot of the player holding the disc, or -1: the present player whose handle { slot, generation }
// is the disc owner's, the comparison /session makes for each player's possession (0x140156939-0x14015697c,
// `cmp edx, ebx`; and possession[0] / [1] at 0x140157222-0x14015726a).
inline int DiscHolderSlot(const TelemetrySnapshot& snap) {
  for (uint8_t i = 0; i < nevr_telemetry_player::kMaxSlots; i++) {
    const auto& pi = snap.playerInfo[i];
    if (!pi.present) continue;
    const uint32_t handle = static_cast<uint32_t>(i) | (static_cast<uint32_t>(pi.handleGeneration) << 16);
    if (handle == snap.discOwnerHandle) return i;
  }
  return -1;
}

// PlayerState.flags as telemetry v2 packs it (`echo_arena.proto` at the pin, `PlayerState.flags`): bit 0
// stunned, bit 1 invulnerable, bit 2 blocking, bit 3 possession, bit 4 is_emote_playing, bits 5-6 the team
// index (0 blue, 1 orange, 2 spectator), bits 7-31 reserved and zero. The team is the entrant word's bits
// 10-12 (PlayerInfo::teamIndex), never the word itself; a team past 2 is not a player's and sets no bit.
// `possession` is whether this player holds the disc (DiscHolderSlot).
constexpr uint32_t kPlayerFlagStunned = 1u << 0;
constexpr uint32_t kPlayerFlagInvulnerable = 1u << 1;
constexpr uint32_t kPlayerFlagBlocking = 1u << 2;
constexpr uint32_t kPlayerFlagPossession = 1u << 3;
constexpr uint32_t kPlayerFlagEmotePlaying = 1u << 4;
constexpr uint32_t kPlayerFlagTeamShift = 5;

inline uint32_t PackPlayerFlags(const TelemetrySnapshot::PlayerInfo& pi, bool possession) {
  uint32_t flags = 0;
  if (pi.stunned) flags |= kPlayerFlagStunned;
  if (pi.invulnerable) flags |= kPlayerFlagInvulnerable;
  if (pi.blocking) flags |= kPlayerFlagBlocking;
  if (possession) flags |= kPlayerFlagPossession;
  if (pi.emotePlaying) flags |= kPlayerFlagEmotePlaying;
  if (pi.teamIndex <= nevr_game_offsets::ENTRANT_TEAM_LAST_PLAYER) {
    flags |= static_cast<uint32_t>(pi.teamIndex) << kPlayerFlagTeamShift;
  }
  return flags;
}

inline void SetPose(spatial::v1::Pose* pose, const float (&pos)[3], const float (&rot)[4]) {
  auto* p = pose->mutable_position();
  p->set_x(pos[0]);
  p->set_y(pos[1]);
  p->set_z(pos[2]);
  auto* o = pose->mutable_orientation();
  o->set_x(rot[0]);
  o->set_y(rot[1]);
  o->set_z(rot[2]);
  o->set_w(rot[3]);
}

// The arena frame's state from the snapshot: game status, clock, scores, round, pause, restart requests, the
// disc and its holder, and each present player. Events are not built here (telemetry_events.h). The fields a
// game read gives (round, pause, restart requests, holder, a player's possession bit) are set only for a
// snapshot the game thread read.
inline void BuildArenaFrame(const TelemetrySnapshot& snap, telemetry::v2::EchoArenaFrame* arena) {
  const int holder = snap.gameRead ? DiscHolderSlot(snap) : -1;
  // Game state
  arena->set_game_status(GameStatusFromSymbol(snap.gameStatusHash));
  arena->set_game_clock(snap.gameClock);
  arena->set_blue_points(snap.bluePoints);
  arena->set_orange_points(snap.orangePoints);
  if (snap.gameRead) {
    arena->set_round_number(snap.roundNumber);
    arena->set_pause_state(nevr_telemetry_session::PauseStateFromGame(snap.pauseStateSymbol, snap.pauseTime));
    arena->set_blue_team_restart_request(snap.blueRestartRequest ? 1 : 0);
    arena->set_orange_team_restart_request(snap.orangeRestartRequest ? 1 : 0);
    if (holder >= 0) arena->set_disc_holder_slot(holder);
  }

  // Disc
  auto* disc = arena->mutable_disc();
  SetPose(disc->mutable_pose(), snap.discPos, snap.discOrient);
  auto* discVel = disc->mutable_velocity();
  discVel->set_x(snap.discVel[0]);
  discVel->set_y(snap.discVel[1]);
  discVel->set_z(snap.discVel[2]);
  disc->set_bounce_count(static_cast<uint32_t>(snap.discBounceCount));

  // Players
  for (uint8_t i = 0; i < nevr_telemetry_player::kMaxSlots; i++) {  // i is the game slot
    const auto& pi = snap.playerInfo[i];
    if (!pi.present) continue;

    auto* ps = arena->add_players();
    ps->set_slot(i);
    ps->set_flags(PackPlayerFlags(pi, holder == i));
    ps->set_ping(pi.ping);
    ps->set_packet_loss_ratio(pi.packetLossRatio);
    ps->set_possession_time(snap.stats[i].possessionTime);

    // Poses, as the game's readers gave them (telemetry_player_pose.h); a part not read is zero
    const auto& pt = snap.players[i];
    SetPose(ps->mutable_head(), pt.headPos, pt.headRot);
    SetPose(ps->mutable_body(), pt.bodyPos, pt.bodyRot);
    SetPose(ps->mutable_left_hand(), pt.leftHandPos, pt.leftHandRot);
    SetPose(ps->mutable_right_hand(), pt.rightHandPos, pt.rightHandRot);
    auto* vel = ps->mutable_velocity();
    vel->set_x(pt.velocity[0]);
    vel->set_y(pt.velocity[1]);
    vel->set_z(pt.velocity[2]);

    // No player_bones: the snapshot holds no bone data (telemetry_snapshot.h).
  }
}

}  // namespace nevr_telemetry_frame
