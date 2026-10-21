#pragma once

#include <chrono>
#include <string>

#include "runtime/server/telemetry_snapshot.h"
#include "telemetry/v2/capture.pb.h"

namespace nevr_game_server {

// The engine's game-type symbol the streamer records in CaptureHeader.game_type. The streamer knows only
// whether the match is private and assumes Echo Arena, as it does for map_name.
const char* TelemetryGameType(bool isPrivateMatch);

// The first envelope of a capture: CaptureHeader with the Echo Arena header and the initial roster from the
// snapshot (slots with account id 0 are skipped, at most 16 players).
telemetry::v2::Envelope BuildCaptureHeaderEnvelope(const std::string& sessionId, bool isPrivateMatch,
                                                   const TelemetrySnapshot& snap,
                                                   std::chrono::system_clock::time_point now);

}  // namespace nevr_game_server
