#include "runtime/server/telemetry_header.h"

#include <cstdint>

#include "telemetry/v2/echo_arena.pb.h"

namespace nevr_game_server {

const char* TelemetryGameType(bool isPrivateMatch) {
  return isPrivateMatch ? "echo_arena_private" : "echo_arena";
}

telemetry::v2::Envelope BuildCaptureHeaderEnvelope(const std::string& sessionId, bool isPrivateMatch,
                                                   const TelemetrySnapshot& snap,
                                                   std::chrono::system_clock::time_point now) {
  telemetry::v2::Envelope envelope;
  auto* header = envelope.mutable_header();

  header->set_capture_id(sessionId);
  header->set_format_version(2);
  header->set_game_type(TelemetryGameType(isPrivateMatch));

  auto* ts = header->mutable_created_at();
  const auto sinceEpoch = now.time_since_epoch();
  const auto secs = std::chrono::duration_cast<std::chrono::seconds>(sinceEpoch);
  const auto nanos = std::chrono::duration_cast<std::chrono::nanoseconds>(sinceEpoch - secs);
  ts->set_seconds(secs.count());
  ts->set_nanos(static_cast<int32_t>(nanos.count()));

  auto* ah = header->mutable_echo_arena();
  ah->set_session_id(sessionId);
  ah->set_client_name("nevr-runtime");
  ah->set_map_name("mpl_arena_a");
  ah->set_total_round_count(3);

  // Skeleton layout (fixed for EchoVR: 23 bones)
  auto* skel = ah->mutable_skeleton();
  skel->set_bone_count(23);
  skel->set_transform_stride(12);    // 3 x float32
  skel->set_orientation_stride(16);  // 4 x float32

  // Initial roster from snapshot
  for (uint16_t i = 0; i < snap.playerCount && i < 16; i++) {
    const auto& pi = snap.playerInfo[i];
    if (pi.accountId == 0) continue;
    auto* entry = ah->add_initial_roster();
    entry->set_slot(i);
    entry->set_account_number(pi.accountId);
    entry->set_display_name(pi.displayName);
    // teamIndex: 0=blue->ROLE_BLUE_TEAM(1), 1=orange->ROLE_ORANGE_TEAM(2), 2=spec->ROLE_SPECTATOR(3)
    entry->set_role(static_cast<telemetry::v2::Role>(pi.teamIndex + 1));
  }
  return envelope;
}

}  // namespace nevr_game_server
