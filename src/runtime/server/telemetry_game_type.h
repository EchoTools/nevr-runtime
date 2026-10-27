#pragma once

// The game type a telemetry capture header carries (CaptureHeader.game_type, telemetry/v2/capture.proto).
//
// The schema's rule: the engine's own game-type symbol, verbatim and lowercase ("echo_arena",
// "echo_arena_private"), never an enum. The runtime does not read the engine's symbol yet; until it does, the
// game type of a session follows its lobby type, which is what the removed match_type and private_match fields
// were derived from.

#include <string_view>

#include "telemetry/v2/capture.pb.h"

namespace nevr_telemetry_header {

// "echo_arena_private" for a private lobby, "echo_arena" for a public one.
std::string_view GameTypeForLobby(bool isPrivateMatch);

// Sets header->game_type() for the session's lobby type.
void SetCaptureGameType(telemetry::v2::CaptureHeader* header, bool isPrivateMatch);

}  // namespace nevr_telemetry_header
