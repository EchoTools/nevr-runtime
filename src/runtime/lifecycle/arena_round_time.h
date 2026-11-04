#pragma once

// The game type of the server's current session. It is stored from the two messages that open a session, whichever the
// server's socket delivers first: the protobuf kLobbySessionCreate and the legacy GameServerSessionStart frame that the
// runtime forwards to the game (gameserver_callbacks.cpp, StoreSessionGameType, which logs each store with the type it
// replaces and sets the session's step from the type, #1785). The Arena round length is not decided from it: round_time
// is the game's own value unless config.yaml sets arena.round_time (lifecycle/arena_rules_hook.cpp).

#include <optional>
#include <string>
#include <string_view>

namespace nevr_arena_round_time {

// Stores the current session's game type and returns the one it replaces (nullopt for the first session of the
// process). Called on every kLobbySessionCreate and every legacy session start, telemetry or not.
std::optional<std::string> SetSessionGameType(std::string_view gameType);

// The stored game type, or nullopt when no session has been created in this process.
std::optional<std::string> SessionGameType();

#ifdef NEVR_ARENA_ROUND_TIME_TESTING
// Forgets the stored game type, so a test starts from a process that has seen no session.
void ResetForTest();
#endif

}  // namespace nevr_arena_round_time
