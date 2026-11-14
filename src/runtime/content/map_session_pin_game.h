#pragma once

// The session map pin's PC half (core/map_session_pin.h is the pure part): the owner-JSON calls into echovr.exe, the error block,
// the return to lobby and the Log() lines. Calls into the game only; the one write the game gets is the error block (WriteErrorBlock).

#include <cstdint>

namespace nevr_map_session_pin {

// The session's level, the owner JSON's "level" or the main level (ADR 0048 decision 9; the PC's main-level source for ConfigAlias).
// Game thread only. Not wired to a caller yet: the channel's start (a later piece) installs it.
std::uint64_t SessionLevel();

// One tick of the pin on the game thread. Not wired to a caller yet (see SessionLevel).
void Tick();

}  // namespace nevr_map_session_pin
