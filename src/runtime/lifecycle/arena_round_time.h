#pragma once

// The public Arena round length on a server (#982). The game reads its round_time from the arena rules JSON
// through CJson_GetFloat. The runtime answers 480 s (8:00) for that read only when the current session is public
// Arena. The session's game type is stored at kLobbySessionCreate (gameserver_callbacks.cpp), and a read before any
// session is known keeps the game's value.

#include <optional>
#include <string>
#include <string_view>

namespace nevr_arena_round_time {

// The engine's symbol for public Arena (telemetry_game_type.cpp: GameTypeForLobby(false)). Private Arena and every
// other game type keep the game's value.
inline constexpr std::string_view kPublicArenaGameType = "echo_arena";

struct RoundTimeDecision {
  bool applied{false};
  std::string_view reason;  // "applied", "no_session_type" or "non_arena"
};

// Stores the current session's game type. Called on every kLobbySessionCreate, telemetry or not.
void SetSessionGameType(std::string_view gameType);

// The stored game type, or nullopt when no session has been created in this process.
std::optional<std::string> SessionGameType();

// Decides one round_time read on a server that has no arena.round_time in config.yaml.
RoundTimeDecision DecideRoundTimeDefault(const std::optional<std::string>& sessionGameType);

}  // namespace nevr_arena_round_time
