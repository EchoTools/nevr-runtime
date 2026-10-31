#include <winsock2.h>
#include <windows.h>

#include <cstring>
#include <mutex>
#include <optional>
#include <string>

#include "abi/echovr_functions.h"
#include "core/globals.h"
#include "core/logging.h"
#include "runtime/lifecycle/arena_round_time.h"
#include "runtime/lifecycle/cli.h"
#include "runtime/lifecycle/config.h"

// The CJson_GetFloat detour that overrides the Arena rules the game reads (celebration time, round time, mercy
// score). It lives apart from config.cpp so a test links it without the config loader (tests/test_arena_rules_hook.cpp).

namespace {

/// Logs the round_time decision when it changes (applied, or the reason it was not), so one server run shows the
/// order of the session's game type and the reads (#982). Unchanged decisions are not logged again.
void LogRoundTimeDecision(const nevr_arena_round_time::RoundTimeDecision& decision,
                          const std::optional<std::string>& gameType, const CHAR* path, FLOAT gameDefault) {
  static std::mutex s_lastMutex;
  static std::string s_lastKey;
  const std::string key = std::string(decision.reason) + "|" + gameType.value_or("");
  std::lock_guard<std::mutex> lock(s_lastMutex);
  if (key == s_lastKey) return;
  s_lastKey = key;
  Log(EchoVR::LogLevel::Info,
      "[NEVR.PATCH] arena_round_time default=%.0fs applied=%d reason=%s game_type=%s path=%s game_default=%.0fs",
      nevr_arena_round_time::kPublicRoundTimeSeconds, decision.applied ? 1 : 0, std::string(decision.reason).c_str(),
      gameType ? gameType->c_str() : "none", path, gameDefault);
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
      return g_arenaCelebrationTime;
    }
    // round_time (but not round_time_private). A server without arena.round_time starts a public Arena
    // session at 8:00 (#982); the _private paths, every other game type and a read before any session keep the
    // game's value.
    if (strstr(path, "round_time") != NULL && strstr(path, "_private") == NULL &&
        strstr(path, "round_timer") == NULL && strstr(path, "sudden_death_round_time") == NULL) {
      if (g_arenaRoundTime > 0.0f) return g_arenaRoundTime;
      if (g_isServer) {
        const std::optional<std::string> gameType = nevr_arena_round_time::SessionGameType();
        const auto decision = nevr_arena_round_time::DecideRoundTimeDefault(gameType);
        if (!gameType) nevr_arena_round_time::NoteReadWithoutType();
        LogRoundTimeDecision(decision, gameType, path, result);
        if (decision.applied) return nevr_arena_round_time::kPublicRoundTimeSeconds;
      }
    }
    // mercy_win_point_spread (but not the _private variant)
    if (g_arenaMercyScore > 0.0f && strstr(path, "mercy_win_point_spread") != NULL &&
        strstr(path, "_private") == NULL) {
      return g_arenaMercyScore;
    }
  }

  return result;
}
