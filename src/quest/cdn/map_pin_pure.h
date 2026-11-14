#pragma once

// The Quest half's decisions, pure: no libr15 symbol and no sentinel call. map_pin_game.cpp calls them with the live addresses;
// the host test (src/quest/tests/map_session_pin_quest_test.cpp) calls them on buffers.

#include <cstddef>
#include <cstdint>

#include "core/map_session_pin.h"
#include "runtime/lifecycle/return_to_lobby_queue.h"

namespace nevr_quest::maps::pin {

// Schedules the game's return to lobby only when the game's call queue exists (#730, runtime/lifecycle/return_to_lobby_queue.h):
// the game's function faults without one. Returns false, and calls nothing, otherwise; the core then retries and reports
// leave_failed, as on the PC.
template <typename Schedule>
bool ScheduleWhenQueueReady(void* netGame, Schedule schedule) {
  if (nevr_return_to_lobby_queue::Check(netGame) != nevr_return_to_lobby_queue::Verdict::Ready) return false;
  schedule(netGame);
  return true;
}

// The libr15 function at image_base+vaddr has its pinned bytes (exact). False, with the record's fields for `reason` and `name`
// in `refusal`, when they differ.
inline bool CheckLibr15Entry(const unsigned char* image_base, std::uint64_t vaddr, const unsigned char* expected, std::size_t n,
                             const char* reason, const char* name, nevr_map_session_pin::PinFields& refusal) {
  const unsigned char* at = image_base + vaddr;
  return nevr_map_session_pin::CheckGameBytes(at, static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(at)), expected, n,
                                              nevr_map_session_pin::SameBytesExact, reason, name, refusal);
}

}  // namespace nevr_quest::maps::pin
