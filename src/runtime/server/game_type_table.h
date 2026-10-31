#pragma once

// The service's game types and the reader of a session's `gametype` (#1493, #1423).
//
// The community service writes `gametype` as the CSymbol64Hash of the mode's name, a signed 64-bit JSON integer
// (LobbySessionSettings.Mode in its server/evr/gameserver_session_start.go), never a string. The telemetry header
// (server/telemetry_game_type.cpp) and the bot rewrite (patch/server_bots.h) read it through this one table.

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
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

/// The fixed part of the service's GameServerSessionStart frame before its settings string: two GUIDs, player limit,
/// entrant count, lobby type and a pad (patch/server_bots.h describes the layout). The settings JSON follows, ending
/// in a NUL, and the entrants after it.
inline constexpr size_t kSessionStartHeaderSize = 36;

/// The game type a GameServerSessionStart frame carries in its settings, by name, or nullopt when the frame is too
/// short, has no NUL-ended settings string, the settings are not a JSON object, or `gametype` is absent or not one
/// of kGameTypes. It is the game type of the frame the runtime forwards to the game as the session's start, so the
/// session's type can be stored from the same bytes the game starts from.
inline std::optional<std::string> GameTypeOfSessionStart(const uint8_t* frame, size_t size) {
  if (frame == nullptr || size <= kSessionStartHeaderSize) return std::nullopt;
  const uint8_t* settings = frame + kSessionStartHeaderSize;
  const void* nul = std::memchr(settings, 0, size - kSessionStartHeaderSize);
  if (nul == nullptr) return std::nullopt;
  const size_t settingsBytes = static_cast<size_t>(static_cast<const uint8_t*>(nul) - settings);
  const nlohmann::ordered_json doc = nlohmann::ordered_json::parse(settings, settings + settingsBytes, nullptr, false);
  if (!doc.is_object()) return std::nullopt;
  const SessionGameType type = ReadGameType(doc);
  if (type.name == "none" || type.name == "unknown") return std::nullopt;
  return type.name;
}

}  // namespace nevr_game_type_table
