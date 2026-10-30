#include "runtime/server/telemetry_game_type.h"

#include <nlohmann/json.hpp>

#include "runtime/server/game_type_table.h"

namespace nevr_telemetry_header {

namespace {

// A game-type symbol is the engine's lowercase name: letters, digits and underscores, never empty.
bool IsSymbolName(const std::string& value) {
  if (value.empty()) return false;
  for (char c : value) {
    const bool ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
    if (!ok) return false;
  }
  return true;
}

}  // namespace

std::string_view GameTypeForLobby(bool isPrivateMatch) {
  return isPrivateMatch ? "echo_arena_private" : "echo_arena";
}

SessionGameType GameTypeForSession(const std::string& settingsJson, bool isPrivateMatch) {
  const nlohmann::ordered_json settings = nlohmann::ordered_json::parse(settingsJson, nullptr, false);
  if (settings.is_object()) {
    const auto it = settings.find("gametype");
    if (it != settings.end() && it->is_string()) {
      const std::string value = it->get<std::string>();
      if (IsSymbolName(value)) return {value, true};
    }
    // The community service sends the mode as its signed symbol, a JSON integer (#1493). The table names it; a
    // number it does not know is the fallback, as before.
    if (it != settings.end() && it->is_number_integer()) {
      const auto named = nevr_game_type_table::ReadGameType(settings);
      if (named.name != "unknown") return {named.name, true};
    }
  }
  return {std::string(GameTypeForLobby(isPrivateMatch)), false};
}

void SetCaptureGameType(telemetry::v2::CaptureHeader* header, std::string_view gameType) {
  header->set_game_type(std::string(gameType));
}

}  // namespace nevr_telemetry_header
