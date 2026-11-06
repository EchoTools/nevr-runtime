#pragma once

#include <cstdint>
#include <cstring>

#include "runtime/server/telemetry_player_read.h"
#include "runtime/server/telemetry_snapshot.h"

// The session values the stream sends that are not a player's, read where the game's /session writer
// (echovr.exe 34.4.631547.1, 0x1401b2ea0) reads them: the level name from the object at *(CR15NetGame + 0x40),
// and replicated variables of the net game through the game's own getters. Game thread only; the getters come
// in as function pointers (a test passes fakes), and the byte reads read only what they are given.
namespace nevr_telemetry_session {

// The getters this reader calls, each with the net game as the owner, as /session calls them.
struct SessionPropertyGetters {
  GetIntPropertyFn i32;
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

// Fills the session fields of a snapshot from `netGame`. A getter that is absent leaves its field 0.
inline void ReadSessionState(const SessionPropertyGetters& getters, void* netGame, TelemetrySnapshot* snap) {
  ReadLevelName(static_cast<const uint8_t*>(netGame), snap->mapName);
  if (getters.i32) {
    snap->totalRoundCount = getters.i32(netGame, nevr_property_hash::TOTAL_ROUND_COUNT);
  }
}

}  // namespace nevr_telemetry_session
