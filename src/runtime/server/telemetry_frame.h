#pragma once

#include <cstdint>
#include <string>

#include "runtime/server/telemetry_player_read.h"
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
  }
}

}  // namespace nevr_telemetry_frame
