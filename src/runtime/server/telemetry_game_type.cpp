#include "runtime/server/telemetry_game_type.h"

namespace nevr_telemetry_header {

std::string_view GameTypeForLobby(bool isPrivateMatch) {
  return isPrivateMatch ? "echo_arena_private" : "echo_arena";
}

void SetCaptureGameType(telemetry::v2::CaptureHeader* header, bool isPrivateMatch) {
  header->set_game_type(std::string(GameTypeForLobby(isPrivateMatch)));
}

}  // namespace nevr_telemetry_header
