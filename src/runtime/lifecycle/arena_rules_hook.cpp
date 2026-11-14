#include <winsock2.h>
#include <windows.h>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <exception>
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
// line says only that config.yaml's value replaced the key while that map's rules are live: it does not claim the value the
// game read came from the map's record, and it carries no map value, because the record this detour was called on (`root`)
// is not shown to be the aliased one.
//
// Why no root test (ReVault, project echovr, read at the tree's pinned build): the record the lookup (ResolveConfigHash
// 0x140D0B170) returns reaches this hook only where a caller hands it to CJson_GetFloat (0x1405FCA60) as the root, which
// CJsonInspectorRead::ReadFloat does at 0x140174FAD with the context's root. The one place that reads mp_arena_rules by
// name (string 0x1416DCF28, its only reference 0x1401B2D1C in SettingsUpdateCB 0x1401B2A90) does not pass the record on:
// it passes the symbol to HasConfigEntry 0x140CEC970 and to GetConfigInt 0x140D055F0, for the zone_N_point_value keys.
// The records SettingsUpdateCB does pass to an Inspect read are the ones it looked up under mp_frisbee_settings
// (0x1416DCE20, via 0x14013F9C0) and mp_fullscreen_settings (0x1416DCEA0, via 0x140134310). No call site was found that
// reads the three keys from the arena record the lookup answered. To settle it in a live run, a Debug record per map and
// key per live epoch carries `root` beside the lookup's answer (`live_record`) and whether they are equal.

namespace {

// The three keys the override answers, named as the log names them.
constexpr const char* kCelebrationKey = "point_score_celebration_time";
constexpr const char* kRoundTimeKey = "round_time";
constexpr const char* kMercyKey = "mercy_win_point_spread";

std::mutex g_overrideMutex;
std::uint64_t g_overrideEpoch = 0;       // the arena alias's live epoch the set below was built in
std::set<std::string> g_overrideLogged;  // "<map id> <key>" (Info) and "root <map id> <key>" (Debug) logged in that epoch
std::atomic<bool> g_overrideFailLogged{false};

// config.yaml's value (`configValue`) replaced the game's read of `key` on the record `root`. While an alias is live, logs at
// Info one line per map and key per epoch of the alias going live (nevr_config_alias::LiveEpoch; a level unloaded and
// loaded again is a new epoch even under one mount), and at Debug, with the same limit, `root` beside the lookup's answer.
// With no alias live nothing is logged.
//
// This runs inside a detour of the game's CJson_GetFloat, so nothing may unwind out of it: an allocation that fails here
// (the id string, the set node) skips the line, logs one Warning for the process, and the caller still returns the
// config.yaml value.
void NoteAliasOverride(const void* root, const char* key, FLOAT configValue) noexcept {
  try {
    const std::string map = nevr_config_alias::LiveMapId(nevr_config_alias::kArenaRulesKey);
    if (map.empty()) return;  // no alias is live: the stock rules play and no map is involved
    const std::uint64_t epoch = nevr_config_alias::LiveEpoch(nevr_config_alias::kArenaRulesKey);
    std::lock_guard<std::mutex> lock(g_overrideMutex);
    if (epoch != g_overrideEpoch) {
      g_overrideEpoch = epoch;
      g_overrideLogged.clear();
    }
    if (g_overrideLogged.insert(map + " " + key).second) {
      Log(EchoVR::LogLevel::Info,
          "[NEVR.MAPS] config_alias override key=%s map=%s config_value=%.3f (config.yaml's arena rule overrides this "
          "map's alias for this key, so the host's value plays)",
          key, map.c_str(), static_cast<double>(configValue));
    }
    if (g_overrideLogged.insert("root " + map + " " + key).second) {
      const void* live = nevr_config_alias::LiveRecord();
      Log(EchoVR::LogLevel::Debug,
          "[NEVR.MAPS] config_alias root_check key=%s map=%s root=0x%llx live_record=0x%llx equal=%s (the record the "
          "game read from, beside the record the lookup answered for the map)",
          key, map.c_str(), static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(root)),
          static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(live)), root == live ? "true" : "false");
    }
  } catch (const std::exception&) {
    if (!g_overrideFailLogged.exchange(true)) {
      try {
        Log(EchoVR::LogLevel::Warning,
            "[NEVR.MAPS] config_alias reason=override_log_failed key=%s (an allocation failed; the override line is "
            "skipped and config.yaml's value is still returned)",
            key);
      } catch (const std::exception&) {
        // The Warning could not be built either; the value is returned all the same.
      }
    }
  }
}

FLOAT Configured(const void* root, const char* key, FLOAT configValue) noexcept {
  NoteAliasOverride(root, key, configValue);
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
      return Configured(root, kCelebrationKey, g_arenaCelebrationTime);
    }
    // round_time (but not round_time_private)
    if (g_arenaRoundTime > 0.0f && strstr(path, "round_time") != NULL && strstr(path, "_private") == NULL &&
        strstr(path, "round_timer") == NULL && strstr(path, "sudden_death_round_time") == NULL) {
      return Configured(root, kRoundTimeKey, g_arenaRoundTime);
    }
    // mercy_win_point_spread (but not the _private variant)
    if (g_arenaMercyScore > 0.0f && strstr(path, "mercy_win_point_spread") != NULL &&
        strstr(path, "_private") == NULL) {
      return Configured(root, kMercyKey, g_arenaMercyScore);
    }
  }

  return result;
}
