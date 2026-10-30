#pragma once

// The game type a telemetry capture header carries (CaptureHeader.game_type, telemetry/v2/capture.proto).
//
// The schema's rule: the engine's own game-type symbol, verbatim and lowercase ("echo_arena",
// "echo_arena_private", "echo_combat"), never an enum. The engine reads that symbol from the session's
// settings JSON: CR15NetClientLobby::LobbySessionStartingCB (echovr.exe 0x1401AD040) looks up the "gametype"
// key with OwnerJson_GetSymbol. The runtime forwards the same settings JSON in LobbySessionCreate, so it reads
// the key from there. The service sends the key as the mode's CSymbol64Hash, a JSON integer, which is read
// against the table in server/game_type_table.h. When the key is absent or names no known mode, the game type
// falls back to the lobby type.

#include <string>
#include <string_view>

#include "telemetry/v2/capture.pb.h"

namespace nevr_telemetry_header {

// The fallback when the settings name no game type: "echo_arena_private" for a private lobby,
// "echo_arena" for a public one.
std::string_view GameTypeForLobby(bool isPrivateMatch);

struct SessionGameType {
  std::string gameType;
  bool fromSettings{false};  // true when the value is the settings JSON's "gametype", false when it is the fallback
};

// The game type of a session: the settings JSON's "gametype" when it names a symbol, else GameTypeForLobby.
SessionGameType GameTypeForSession(const std::string& settingsJson, bool isPrivateMatch);

// Sets header->game_type() to the session's game type.
void SetCaptureGameType(telemetry::v2::CaptureHeader* header, std::string_view gameType);

}  // namespace nevr_telemetry_header
