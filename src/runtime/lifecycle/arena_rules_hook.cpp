#include <winsock2.h>
#include <windows.h>

#include <cstring>

#include "abi/echovr_functions.h"
#include "core/globals.h"
#include "runtime/lifecycle/config.h"

// The CJson_GetFloat detour that overrides the Arena rules the game reads (celebration time, round time, mercy
// score). It lives apart from config.cpp so a test links it without the config loader (tests/test_arena_rules_hook.cpp).

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
    // round_time (but not round_time_private)
    if (g_arenaRoundTime > 0.0f && strstr(path, "round_time") != NULL && strstr(path, "_private") == NULL &&
        strstr(path, "round_timer") == NULL && strstr(path, "sudden_death_round_time") == NULL) {
      return g_arenaRoundTime;
    }
    // mercy_win_point_spread (but not the _private variant)
    if (g_arenaMercyScore > 0.0f && strstr(path, "mercy_win_point_spread") != NULL &&
        strstr(path, "_private") == NULL) {
      return g_arenaMercyScore;
    }
  }

  return result;
}
