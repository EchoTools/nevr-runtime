#pragma once

// The one check that a game function's entry still has its stock head, or a hook's jmp there whose tail is still stock. Used
// by every caller that calls a game function by its address (veilnet.cpp, map_session_pin_game.cpp) before it calls it.

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace nevr_prologue_head {

// A hook writes a 5-byte jmp (opcode 0xE9 and a rel32) over the entry; the stock bytes follow it.
inline constexpr std::uint8_t kJmpOpcode = 0xE9;
inline constexpr std::size_t kJmpBytes = 5;
// A jmp-hooked entry is only compared past the jmp when the head is longer than the jmp itself.
inline constexpr std::size_t kMinHookedHead = 8;

// True when the n bytes at `entry` are the stock head `head`, or a jmp at `entry` whose bytes after the jmp are the stock
// head's bytes after it (another hook sits there; the game's own calls go through it).
inline bool EntryOk(const std::uint8_t* entry, const std::uint8_t* head, std::size_t n) {
  if (std::memcmp(entry, head, n) == 0) return true;
  return entry[0] == kJmpOpcode && n > kMinHookedHead &&
         std::memcmp(entry + kJmpBytes, head + kJmpBytes, n - kJmpBytes) == 0;
}

}  // namespace nevr_prologue_head
