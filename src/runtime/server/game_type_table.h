#pragma once

// The service's game types and the reader of a session's `gametype` (#1493, #1423).
//
// The community service writes `gametype` as the CSymbol64Hash of the mode's name, a signed 64-bit JSON integer
// (LobbySessionSettings.Mode in its server/evr/gameserver_session_start.go), never a string. The telemetry header
// (server/telemetry_game_type.cpp) and the bot rewrite (patch/server_bots.h) read it through this one table.

#include <cstdint>
#include <string>

#include <nlohmann/json.hpp>

#include "nevr/symbol_hash.h"

namespace nevr_game_type_table {

struct GameType {
  const char* name;
  uint64_t symbol;
  bool bots;  // a match that gets bots
};

/// The service's eleven game types (AllModes in its server/evr/gameserver_session_start.go): the arena and combat
/// matches get bots, the social lobbies none. echo_arena_practice is an arena match like echo_arena_public_ai.
inline constexpr GameType kGameTypes[] = {
    {"echo_arena", EchoVR::CSymbol64Hash("echo_arena"), true},
    {"echo_arena_private", EchoVR::CSymbol64Hash("echo_arena_private"), true},
    {"echo_arena_tournament", EchoVR::CSymbol64Hash("echo_arena_tournament"), true},
    {"echo_arena_public_ai", EchoVR::CSymbol64Hash("echo_arena_public_ai"), true},
    {"echo_arena_practice", EchoVR::CSymbol64Hash("echo_arena_practice"), true},
    {"echo_combat", EchoVR::CSymbol64Hash("echo_combat"), true},
    {"echo_combat_private", EchoVR::CSymbol64Hash("echo_combat_private"), true},
    {"echo_combat_tournament", EchoVR::CSymbol64Hash("echo_combat_tournament"), true},
    {"social_2.0", EchoVR::CSymbol64Hash("social_2.0"), false},
    {"social_2.0_private", EchoVR::CSymbol64Hash("social_2.0_private"), false},
    {"social_2.0_npe", EchoVR::CSymbol64Hash("social_2.0_npe"), false},
};

/// The settings' game type: one of kGameTypes, by its symbol (a JSON integer, signed or not: an unsigned one is an
/// integer too, and its int64 read keeps its bits) or its name; "none" when the key is absent, "unknown" for any
/// other value. Only a match gets bots.
struct SessionGameType {
  std::string name = "none";
  bool bots = false;
};
inline SessionGameType ReadGameType(const nlohmann::ordered_json& settings) {
  SessionGameType out;
  const auto it = settings.find("gametype");
  if (it == settings.end()) return out;
  out.name = "unknown";
  for (const GameType& t : kGameTypes) {
    const bool same = it->is_number_integer() ? static_cast<uint64_t>(it->get<int64_t>()) == t.symbol
                      : it->is_string()         ? it->get<std::string>() == t.name
                                                : false;
    if (same) {
      out.name = t.name;
      out.bots = t.bots;
    }
  }
  return out;
}

}  // namespace nevr_game_type_table
