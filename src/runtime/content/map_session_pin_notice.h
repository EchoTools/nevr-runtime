#pragma once

// Where the session pin's notice is written on the PC: the error block of the CR15NetGame, not echovr.exe's image
// (runtime/patch/lobby_failure_message_rules.h: WriteErrorBlock writes at base + kErrorBlockOffset, and the block lives on the net
// game). Pure: no game call, so the test links nothing of the game half (map_session_pin_game.cpp).

#include <cstdint>

#include "runtime/lifecycle/return_to_lobby_queue.h"

namespace nevr_map_session_pin_notice {

// The net game of the game context, the base WriteErrorBlock takes. Null when the context has no net game yet, and then
// WriteErrorBlock declines (returns false) and the core records notice_not_written.
inline std::uint8_t* NoticeBaseFor(void* context) {
  return static_cast<std::uint8_t*>(nevr_return_to_lobby_queue::NetGameFromContext(context));
}

}  // namespace nevr_map_session_pin_notice
