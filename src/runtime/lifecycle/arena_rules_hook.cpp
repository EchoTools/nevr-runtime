#include <winsock2.h>
#include <windows.h>

#include <cstring>
#include <mutex>
#include <set>
#include <string>

#include "abi/echovr_functions.h"
#include "core/config_alias.h"
#include "core/globals.h"
#include "core/logging.h"
#include "runtime/lifecycle/config.h"

// The CJson_GetFloat detour that overrides the Arena rules the game reads (celebration time, round time, mercy
// score). It lives apart from config.cpp so a test links it without the config loader (tests/test_arena_rules_hook.cpp).
//
// Precedence (docs/adr/0048 decision 5, decided): config.yaml's arena value wins over a map's aliased rule. The alias
// picks which record the game reads (runtime/content/config_alias_hook.cpp); this detour then replaces the value it read
// by its path. While a map's alias for the arena rules is live, the first override of each key per map and per load is
// logged at Info, so the operator sees that the host's value replaced the map's.

namespace {

// The three keys the override answers, named as the log names them.
constexpr const char* kCelebrationKey = "point_score_celebration_time";
constexpr const char* kRoundTimeKey = "round_time";
constexpr const char* kMercyKey = "mercy_win_point_spread";

std::mutex g_overrideMutex;
std::uint64_t g_overrideGeneration = 0;  // the alias table's generation the set below was built in
std::set<std::string> g_overrideLogged;  // "<map id> <key>" logged in that generation

// The override replaced a value the game read (`mapValue`) with config.yaml's (`configValue`). Logs one line per map and
// key per load; a load is one generation of the alias table (Stage, Unstage and Arm bump it on each mount).
void NoteAliasOverride(const char* key, FLOAT mapValue, FLOAT configValue) {
  const std::string map = nevr_config_alias::LiveMapId(nevr_config_alias::kArenaRulesKey);
  if (map.empty()) return;  // no alias is live: the game read the stock record, and nothing is replaced from a map
  const std::uint64_t generation = nevr_config_alias::Global().Generation();
  std::lock_guard<std::mutex> lock(g_overrideMutex);
  if (generation != g_overrideGeneration) {
    g_overrideGeneration = generation;
    g_overrideLogged.clear();
  }
  if (!g_overrideLogged.insert(map + " " + key).second) return;
  Log(EchoVR::LogLevel::Info,
      "[NEVR.MAPS] config_alias override key=%s map=%s map_value=%.3f config_value=%.3f (config.yaml's arena rule "
      "replaces this map's rule for this key, so the host's value plays)",
      key, map.c_str(), static_cast<double>(mapValue), static_cast<double>(configValue));
}

FLOAT Configured(const char* key, FLOAT mapValue, FLOAT configValue) {
  NoteAliasOverride(key, mapValue, configValue);
  return configValue;
}

}  // namespace

/// <summary>
/// Hook for CJson_GetFloat to override arena rule config values at load time.
/// Matches specific JSON path suffixes and returns overridden values from config.json.
/// </summary>
FLOAT CJsonGetFloatHook(PVOID root, const CHAR* path, FLOAT defaultValue, INT32 required) {
  FLOAT result = EchoVR::CJsonGetFloat(root, path, defaultValue, required);

  if (path != NULL) {
    // point_score_celebration_time (but not the _private variant)
    if (g_arenaCelebrationTime > 0.0f && strstr(path, "point_score_celebration_time") != NULL &&
        strstr(path, "_private") == NULL) {
      return Configured(kCelebrationKey, result, g_arenaCelebrationTime);
    }
    // round_time (but not round_time_private)
    if (g_arenaRoundTime > 0.0f && strstr(path, "round_time") != NULL && strstr(path, "_private") == NULL &&
        strstr(path, "round_timer") == NULL && strstr(path, "sudden_death_round_time") == NULL) {
      return Configured(kRoundTimeKey, result, g_arenaRoundTime);
    }
    // mercy_win_point_spread (but not the _private variant)
    if (g_arenaMercyScore > 0.0f && strstr(path, "mercy_win_point_spread") != NULL &&
        strstr(path, "_private") == NULL) {
      return Configured(kMercyKey, result, g_arenaMercyScore);
    }
  }

  return result;
}
