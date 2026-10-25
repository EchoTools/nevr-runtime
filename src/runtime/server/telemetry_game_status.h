#pragma once

#include <cstdint>

#include "runtime/server/telemetry_snapshot.h"
#include "telemetry/v2/echo_arena.pb.h"

// The game's state symbol (the value of its `game_state` replicated variable) to the telemetry
// enum. The groups follow the game's own state-to-string function, echovr.exe 0x14016d510, which
// GameState_SerializeToJson calls at 0x140155dca.
inline telemetry::v2::GameStatus GameStatusFromSymbol(uint64_t symbol) {
  using namespace nevr_game_status_hash;
  switch (symbol) {
    case PRE_MATCH_1:
    case PRE_MATCH_2:
    case PRE_MATCH_3:
      return telemetry::v2::GAME_STATUS_PRE_MATCH;
    case ROUND_START_1:
    case ROUND_START_2:
    case ROUND_START_3:
    case ROUND_START_4:
    case ROUND_START_5:
      return telemetry::v2::GAME_STATUS_ROUND_START;
    case PLAYING_1:
    case PLAYING_2:
      return telemetry::v2::GAME_STATUS_PLAYING;
    case SCORE_1:
    case SCORE_2:
      return telemetry::v2::GAME_STATUS_SCORE;
    case ROUND_OVER_1:
    case ROUND_OVER_2:
      return telemetry::v2::GAME_STATUS_ROUND_OVER;
    case POST_MATCH_1:
    case POST_MATCH_2:
    case POST_MATCH_3:
    case POST_MATCH_4:
    case POST_MATCH_5:
      return telemetry::v2::GAME_STATUS_POST_MATCH;
    case PRE_SUDDEN_DEATH_1:
    case PRE_SUDDEN_DEATH_2:
      return telemetry::v2::GAME_STATUS_PRE_SUDDEN_DEATH;
    case SUDDEN_DEATH_1:
    case SUDDEN_DEATH_2:
      return telemetry::v2::GAME_STATUS_SUDDEN_DEATH;
    case POST_SUDDEN_DEATH_1:
    case POST_SUDDEN_DEATH_2:
      return telemetry::v2::GAME_STATUS_POST_SUDDEN_DEATH;
    default:
      return telemetry::v2::GAME_STATUS_UNSPECIFIED;
  }
}
