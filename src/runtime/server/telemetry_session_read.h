#pragma once

#include <cstdint>
#include <cstring>

#include "runtime/server/telemetry_player_read.h"
#include "runtime/server/telemetry_snapshot.h"
#include "telemetry/v2/echo_arena.pb.h"

// The session values the stream sends that are not a player's, read where the game's /session writer
// (echovr.exe 34.4.631547.1, 0x1401b2ea0) reads them: the level name from the object at *(CR15NetGame + 0x40),
// and replicated variables of the net game through the game's own getters. Game thread only; the getters come
// in as function pointers (a test passes fakes), and the byte reads read only what they are given.
namespace nevr_telemetry_session {

// The getters this reader calls, each with the net game as the owner, as /session calls them.
struct SessionPropertyGetters {
  GetIntPropertyFn i32;
  GetSymbolHashFn symbol;
  GetFloatPropertyFn f32;
  GetBoolPropertyFn boolean;
};

// The level name into a snapshot's 64 bytes: at most 63 of them, then a NUL. Empty when there is no object at
// *(netGame + 0x40) (/session writes "" then) or when its name pointer is null (/session does not check it).
inline void ReadLevelName(const uint8_t* netGame, char (&dest)[64]) {
  namespace off = nevr_game_offsets;
  dest[0] = '\0';
  const uint8_t* owner = nevr_telemetry_player::ReadAt<const uint8_t*>(netGame, off::LOBBY_POINTER_OFFSET);
  if (!owner) return;
  const char* name = nevr_telemetry_player::ReadAt<const char*>(owner, off::SESSION_LEVEL_NAME_OFFSET);
  if (!name) return;
  const size_t length = strnlen(name, sizeof(dest) - 1);
  std::memcpy(dest, name, length);
  dest[length] = '\0';
}

// A symbol variable through the symbol getter, -1 when the getter or the variable is absent (the getter
// writes -1 then).
inline uint64_t ReadSymbol(const SessionPropertyGetters& getters, void* netGame, uint64_t key) {
  uint64_t out = ~0ULL;
  if (getters.symbol) getters.symbol(&out, netGame, key);
  return out;
}

// Fills the session fields of a snapshot from `netGame`. A getter that is absent leaves its fields 0, and a
// symbol it would have read -1.
inline void ReadSessionState(const SessionPropertyGetters& getters, void* netGame, TelemetrySnapshot* snap) {
  namespace key = nevr_property_hash;
  ReadLevelName(static_cast<const uint8_t*>(netGame), snap->mapName);
  if (getters.i32) {
    snap->totalRoundCount = getters.i32(netGame, key::TOTAL_ROUND_COUNT);
    snap->roundNumber = getters.i32(netGame, key::ROUND_COUNTER);
  }
  // The owner is a handle { u16 slot, u16 generation } in the low 32 bits, compared whole with a player's
  // (0x140155d95 `mov rdi, [rbp + 0xc8]`, 0x140155dc3 / 0x140155db5 its two halves, 0x14015694a-0x140156957).
  snap->discOwnerHandle = static_cast<uint32_t>(ReadSymbol(getters, netGame, key::DISC_OWNER));
  snap->pauseStateSymbol = ReadSymbol(getters, netGame, key::PAUSED_STATE);
  if (getters.f32) snap->pauseTime = getters.f32(netGame, key::PAUSE_TIME);
  if (getters.boolean) {
    snap->blueRestartRequest = getters.boolean(netGame, key::BLUE_RESTART_REQUEST);
    snap->orangeRestartRequest = getters.boolean(netGame, key::ORANGE_RESTART_REQUEST);
  }
}

// pause.paused_state as /session names it (0x1401b32d1-0x1401b3345), in the stream's enum: "unpaused", or
// "unpausing" while net_game_pause_time is above 0 (0x1401b3336 `comiss xmm6, xmm15`, 0x1401b3341 `cmovbe`:
// a time of 0 or less, or NaN, stays "unpaused"); "paused"; "paused_requested"; anything else "none".
inline telemetry::v2::PauseState PauseStateFromGame(uint64_t symbol, float pauseTime) {
  switch (symbol) {
    case nevr_pause_symbol::UNPAUSED:
      return pauseTime > 0.0f ? telemetry::v2::PAUSE_STATE_UNPAUSING : telemetry::v2::PAUSE_STATE_NOT_PAUSED;
    case nevr_pause_symbol::PAUSED:
      return telemetry::v2::PAUSE_STATE_PAUSED;
    case nevr_pause_symbol::PAUSE_REQUESTED:
      return telemetry::v2::PAUSE_STATE_PAUSED_REQUESTED;
    default:
      return telemetry::v2::PAUSE_STATE_NONE;
  }
}

}  // namespace nevr_telemetry_session
