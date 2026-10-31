#pragma once

// The public Arena round length on a server (#982). The game reads its round_time from the arena rules JSON
// through CJson_GetFloat. The runtime answers 480 s (8:00) for that read only when the current session is public
// Arena. The session's game type is stored from the two messages that open a session, whichever the server's
// socket delivers first: the protobuf kLobbySessionCreate and the legacy GameServerSessionStart frame that the
// runtime forwards to the game (gameserver_callbacks.cpp). The frame is the message the game starts from, so its
// game type is stored before the game sees it, and a later session never reads the previous one's type. A read before
// any session is known keeps the game's value and is counted, so the next session's log line says how many reads
// the type missed.

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace nevr_arena_round_time {

// The engine's symbol for public Arena (telemetry_game_type.cpp: GameTypeForLobby(false)). Private Arena and every
// other game type keep the game's value.
inline constexpr std::string_view kPublicArenaGameType = "echo_arena";

// The round length a public Arena session gets when the server has no arena.round_time in config.yaml.
inline constexpr float kPublicRoundTimeSeconds = 480.0f;

struct RoundTimeDecision {
  bool applied{false};
  std::string_view reason;  // "applied", "no_session_type" or "non_arena"
};

// Stores the current session's game type and returns the one it replaces (nullopt for the first session of the
// process). Called on every kLobbySessionCreate and every legacy session start, telemetry or not.
std::optional<std::string> SetSessionGameType(std::string_view gameType);

// The stored game type, or nullopt when no session has been created in this process.
std::optional<std::string> SessionGameType();

// Counts one round_time read that found no stored game type (the hook calls it on a server).
void NoteReadWithoutType();

// The reads counted since the last call, reset to zero. The session start reports it, so a public Arena session whose
// type arrived after the game's first read is visible in the log with the number of reads it missed.
std::uint32_t TakeReadsWithoutType();

// Decides one round_time read on a server that has no arena.round_time in config.yaml.
RoundTimeDecision DecideRoundTimeDefault(const std::optional<std::string>& sessionGameType);

#ifdef NEVR_ARENA_ROUND_TIME_TESTING
// Forgets the stored game type and the count, so a test starts from a process that has seen no session.
void ResetForTest();
#endif

}  // namespace nevr_arena_round_time
