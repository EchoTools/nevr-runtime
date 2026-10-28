// The running libr15's identity, read from the mapped image: its GNU build ID and its load base. Its own translation unit
// because pinned_targets.h reaches callback_thunk.h, which only -fno-exceptions code may include (the loader itself is built
// with exceptions).
#pragma once

#include <cstddef>
#include <cstdint>

namespace nevr_quest::plugin {

// Lowercase hex of libr15.so's NT_GNU_BUILD_ID into `out`; false when libr15 is not mapped yet or has no note.
bool ReadGameBuildId(char* out, std::size_t capacity) noexcept;

// libr15.so's load base; 0 when it is not mapped.
std::uintptr_t GameBaseAddress() noexcept;

}  // namespace nevr_quest::plugin
