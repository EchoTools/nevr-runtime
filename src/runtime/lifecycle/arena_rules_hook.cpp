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
// Precedence (docs/adr/0048 decision 5): config.yaml's arena value wins over a map's aliased rule. The alias picks which
// record the game reads (runtime/content/config_alias_hook.cpp); this detour then replaces the value it read by its path.
// While a map's alias is live, each override is logged at Info once per map and key each time that alias goes live. The
// line is written only when the value came from the aliased record the lookup answered, not from another record under the
// same path.

namespace {

// The three keys the override answers, named as the log names them.
constexpr const char* kCelebrationKey = "point_score_celebration_time";
constexpr const char* kRoundTimeKey = "round_time";
constexpr const char* kMercyKey = "mercy_win_point_spread";

std::mutex g_overrideMutex;
std::uint64_t g_overrideEpoch = 0;       // the arena alias's live epoch the set below was built in
std::set<std::string> g_overrideLogged;  // "<map id> <key>" logged in that epoch

// The override replaced the value the game read (`mapValue`) with config.yaml's (`configValue`), on the record `root`. Logs
// one line per map and key per epoch of the alias going live (nevr_config_alias::LiveEpoch): a level unloaded and loaded
// again is a new epoch even under one mount. Nothing is logged unless `root` is the aliased record the lookup answered.
void NoteAliasOverride(const void* root, const char* key, FLOAT mapValue, FLOAT configValue) {
  if (root == nullptr || root != nevr_config_alias::LiveRecord()) return;
  const std::string map = nevr_config_alias::LiveMapId(nevr_config_alias::kArenaRulesKey);
  if (map.empty()) return;  // no alias is live: the game read the stock record, and nothing is replaced from a map
  const std::uint64_t epoch = nevr_config_alias::LiveEpoch(nevr_config_alias::kArenaRulesKey);
  std::lock_guard<std::mutex> lock(g_overrideMutex);
  if (epoch != g_overrideEpoch) {
    g_overrideEpoch = epoch;
    g_overrideLogged.clear();
  }
  if (!g_overrideLogged.insert(map + " " + key).second) return;
  Log(EchoVR::LogLevel::Info,
      "[NEVR.MAPS] config_alias override key=%s map=%s map_value=%.3f config_value=%.3f (config.yaml's arena rule "
      "replaces this map's rule for this key, so the host's value plays)",
      key, map.c_str(), static_cast<double>(mapValue), static_cast<double>(configValue));
}

FLOAT Configured(const void* root, const char* key, FLOAT mapValue, FLOAT configValue) {
  NoteAliasOverride(root, key, mapValue, configValue);
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
      return Configured(root, kCelebrationKey, result, g_arenaCelebrationTime);
    }
    // round_time (but not round_time_private)
    if (g_arenaRoundTime > 0.0f && strstr(path, "round_time") != NULL && strstr(path, "_private") == NULL &&
        strstr(path, "round_timer") == NULL && strstr(path, "sudden_death_round_time") == NULL) {
      return Configured(root, kRoundTimeKey, result, g_arenaRoundTime);
    }
    // mercy_win_point_spread (but not the _private variant)
    if (g_arenaMercyScore > 0.0f && strstr(path, "mercy_win_point_spread") != NULL &&
        strstr(path, "_private") == NULL) {
      return Configured(root, kMercyKey, result, g_arenaMercyScore);
    }
  }

  return result;
}
