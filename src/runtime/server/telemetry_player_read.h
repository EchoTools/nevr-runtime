#pragma once

#include <cstdint>
#include <cstring>

#include "abi/echovr.h"
#include "runtime/server/telemetry_snapshot.h"
#include "telemetry/v2/echo_arena.pb.h"

// The players of a net game, read the way the game's own /session writer reads them (echovr.exe
// 34.4.631547.1, players 0x140156380-0x140157800, identity 0x1401b3780-0x1401b3e00). Pure: it reads the
// bytes it is given and calls nothing, so a test drives it with a buffer laid out as the game lays it out.
// SnapshotGameState and RunDiagnostics both read through it.
namespace nevr_telemetry_player {

// TelemetrySnapshot keeps one entry per game slot; a slot at or past this is not read.
constexpr uint8_t kMaxSlots = 16;

template <typename T>
inline T ReadAt(const uint8_t* base, uint64_t offset) {
  T value;
  std::memcpy(&value, base + offset, sizeof(value));
  return value;
}

// The game slot and the team of a net-game entrant, from the first word of its record.
inline uint8_t EntrantSlot(uint16_t word) {
  return static_cast<uint8_t>(word & nevr_game_offsets::ENTRANT_SLOT_MASK);
}
inline uint16_t EntrantTeam(uint16_t word) {
  return static_cast<uint16_t>((word >> nevr_game_offsets::ENTRANT_TEAM_SHIFT) & nevr_game_offsets::ENTRANT_TEAM_MASK);
}

// The stream's role for a player's team (EntrantTeam): 0 blue, 1 orange, 2 spectator, as /session names
// them (0x14015645d-0x140156488). The roster, joins and team switches all map through this.
inline telemetry::v2::Role RoleFromTeam(uint16_t team) {
  switch (team) {
    case 0:
      return telemetry::v2::ROLE_BLUE_TEAM;
    case 1:
      return telemetry::v2::ROLE_ORANGE_TEAM;
    case 2:
      return telemetry::v2::ROLE_SPECTATOR;
    default:
      return telemetry::v2::ROLE_UNSPECIFIED;
  }
}

// One stat of one slot: `value`, or `value / count` when the entry's type is the average type, and 0 for
// an average with no count (0x140156ae8 for a count, 0x140156a17 for a float).
inline uint32_t ReadCountStat(const uint8_t* netGame, uint8_t slot, uint64_t statOffset) {
  const uint8_t* entry = netGame + slot * nevr_game_offsets::SLOT_STATS_STRIDE + statOffset;
  const uint32_t type = ReadAt<uint32_t>(entry, nevr_game_offsets::STAT_ENTRY_TYPE);
  const uint32_t value = ReadAt<uint32_t>(entry, nevr_game_offsets::STAT_ENTRY_VALUE);
  if (type != nevr_game_offsets::STAT_TYPE_AVERAGE) return value;
  const uint32_t count = ReadAt<uint32_t>(entry, nevr_game_offsets::STAT_ENTRY_COUNT);
  return count == 0 ? 0 : value / count;
}

inline float ReadFloatStat(const uint8_t* netGame, uint8_t slot, uint64_t statOffset) {
  const uint8_t* entry = netGame + slot * nevr_game_offsets::SLOT_STATS_STRIDE + statOffset;
  const uint32_t type = ReadAt<uint32_t>(entry, nevr_game_offsets::STAT_ENTRY_TYPE);
  const float value = ReadAt<float>(entry, nevr_game_offsets::STAT_ENTRY_VALUE);
  if (type != nevr_game_offsets::STAT_TYPE_AVERAGE) return value;
  const uint32_t count = ReadAt<uint32_t>(entry, nevr_game_offsets::STAT_ENTRY_COUNT);
  return count == 0 ? 0.0f : value / static_cast<float>(count);
}

inline TelemetrySnapshot::PlayerStats ReadPlayerStats(const uint8_t* netGame, uint8_t slot) {
  namespace off = nevr_game_offsets;
  TelemetrySnapshot::PlayerStats s{};
  s.stuns = ReadCountStat(netGame, slot, off::SLOT_STAT_STUNS);
  s.blocks = ReadCountStat(netGame, slot, off::SLOT_STAT_BLOCKS);
  s.points = ReadCountStat(netGame, slot, off::SLOT_STAT_POINTS);
  s.goals = ReadCountStat(netGame, slot, off::SLOT_STAT_GOALS);
  s.steals = ReadCountStat(netGame, slot, off::SLOT_STAT_STEALS);
  s.assists = ReadCountStat(netGame, slot, off::SLOT_STAT_ASSISTS);
  s.saves = ReadCountStat(netGame, slot, off::SLOT_STAT_SAVES);
  s.shotsTaken = ReadCountStat(netGame, slot, off::SLOT_STAT_SHOTS_TAKEN);
  s.passes = ReadCountStat(netGame, slot, off::SLOT_STAT_PASSES);
  s.catches = ReadCountStat(netGame, slot, off::SLOT_STAT_CATCHES);
  s.interceptions = ReadCountStat(netGame, slot, off::SLOT_STAT_INTERCEPTIONS);
  s.possessionTime = ReadFloatStat(netGame, slot, off::SLOT_STAT_POSSESSION_TIME);
  return s;
}

// A player's jersey number, at the slot (the entrant index is below the entrant count in ReadPlayers' loop,
// so /session's 0 for a later index is not reached there).
inline uint16_t ReadJerseyNumber(const uint8_t* netGame, uint8_t slot) {
  namespace off = nevr_game_offsets;
  return ReadAt<uint16_t>(netGame, off::JERSEY_NUMBER_OFFSET + slot * off::JERSEY_NUMBER_STRIDE);
}

// A player's packet-loss ratio, by entrant index and slot, read where /session reads it (PACKET_LOSS_*). The
// session-flags word is read through *(netGame + 0x2DA0), which /session dereferences unchecked; while that
// pointer is null this gives 0 for the entrants whose ratio depends on it.
inline float ReadPacketLossRatio(const uint8_t* netGame, uint16_t entrantIndex, uint8_t slot) {
  namespace off = nevr_game_offsets;
  if (entrantIndex >= ReadAt<uint16_t>(netGame, off::PLAYER_COUNT_OFFSET)) return 0.0f;
  if (entrantIndex < ReadAt<uint16_t>(netGame, off::REMOTE_ENTRANT_FIRST_OFFSET)) {
    return ReadAt<float>(netGame, off::PACKET_LOSS_LOCAL_OFFSET);
  }
  const uint8_t* flags = ReadAt<const uint8_t*>(netGame, off::SESSION_FLAGS_POINTER_OFFSET);
  if (!flags) return 0.0f;
  const uint8_t first = flags[0];
  const bool wide = (first & off::SESSION_FLAG_BIT1) != 0 ||
                    ((first & off::SESSION_FLAG_BIT2) == 0 && (first & off::SESSION_FLAG_BIT6) != 0);
  if (wide) return ReadAt<float>(netGame, off::PACKET_LOSS_WIDE_OFFSET + slot * off::PACKET_LOSS_WIDE_STRIDE);
  return ReadAt<float>(netGame, off::PACKET_LOSS_NARROW_OFFSET + slot * off::PACKET_LOSS_NARROW_STRIDE);
}

// The lobby's entrant table, indexed by game slot: EchoVR::Lobby::entrantData of the CNSLobby at
// *(netGame + LOBBY_POINTER_OFFSET) + LOBBY_SUBOBJECT_OFFSET, the object IServerLib::Initialize is given and
// ServerContext::GetEntrant reads.
struct LobbyEntrants {
  const EchoVR::Lobby::EntrantData* items = nullptr;
  uint64_t count = 0;
};

inline LobbyEntrants LobbyEntrantsOf(const uint8_t* netGame) {
  const uint8_t* owner = ReadAt<const uint8_t*>(netGame, nevr_game_offsets::LOBBY_POINTER_OFFSET);
  if (!owner) return {};
  const auto* lobby = reinterpret_cast<const EchoVR::Lobby*>(owner + nevr_game_offsets::LOBBY_SUBOBJECT_OFFSET);
  LobbyEntrants entrants;
  entrants.items = lobby->entrantData.items;
  entrants.count = entrants.items ? lobby->entrantData.count : 0;
  return entrants;
}

// The generation in the handle table's entry for a game slot (below kMaxSlots); with the slot it is the
// player's handle (0x140156537), which /session compares with the disc owner's.
inline uint16_t HandleTableGeneration(const uint8_t* netGame, uint8_t slot) {
  namespace off = nevr_game_offsets;
  return ReadAt<uint16_t>(netGame, off::SLOT_HANDLE_TABLE_OFFSET + slot * off::SLOT_HANDLE_STRIDE +
                                       off::SLOT_HANDLE_GENERATION);
}

// The entrant the net game's handle table names for a game slot (below kMaxSlots), or SLOT_HANDLE_NONE when
// the entry is empty: its generation or its entrant index is 0xffff (0x140156557, 0x140156572).
inline uint16_t HandleTableEntrant(const uint8_t* netGame, uint8_t slot) {
  namespace off = nevr_game_offsets;
  const uint64_t entry = off::SLOT_HANDLE_TABLE_OFFSET + slot * off::SLOT_HANDLE_STRIDE;
  const uint16_t generation = ReadAt<uint16_t>(netGame, entry + off::SLOT_HANDLE_GENERATION);
  const uint16_t entrant = ReadAt<uint16_t>(netGame, entry + off::SLOT_HANDLE_ENTRANT);
  if (generation == off::SLOT_HANDLE_NONE) return off::SLOT_HANDLE_NONE;
  return entrant;
}

// What ReadPlayers left out: entrants of a team above 2 (the server's own entrant is team 4), as /session
// does; and players it could not send: a slot at or past kMaxSlots, an entrant its slot's handle-table entry
// does not name (an empty entry, or one that names another entrant: a stale or second record of that slot),
// and a slot with no entry in the lobby's entrant table (no account id or name). The handle table names one
// entrant per slot, so no two entrants are read into one slot.
struct PlayerReadResult {
  uint16_t notPlayers = 0;
  uint16_t slotOutOfRange = 0;
  uint16_t notInHandleTable = 0;
  uint16_t noIdentity = 0;
};

// Copies a name into a snapshot's 36 bytes: at most 35 of them, then a NUL.
inline void CopyName(char (&dest)[36], const char* name) {
  const size_t length = name ? strnlen(name, sizeof(dest) - 1) : 0;
  std::memcpy(dest, name ? name : "", length);
  dest[length] = '\0';
}

// Fills a cleared snapshot's playerInfo and stats by game slot (an absent slot keeps present == false) and
// counts the players read in snap->playerCount. Entrant i is read only when its slot's handle-table entry
// names i, as the /session writer reads the entrant the table names, and i is kept as the slot's entrantIndex. The account id and ping are the lobby
// entrant's at the same slot; a player whose slot is past the lobby's table is left out. The name is what
// getUserName (the game's GetUserName, game thread only) returns for entrant i, which is /session's name.
// Without it (a test, or a caller that has not resolved it), or while the net game's session-flags pointer is
// null (GetUserName would dereference it), the name is the lobby entrant's sfw name, the fallback the game
// itself takes when the entrant's state data has no "displayname". Each player also gets its handle's
// generation, its jersey number, level and packet-loss ratio.
inline PlayerReadResult ReadPlayers(const uint8_t* netGame, TelemetrySnapshot* snap,
                                    GetUserNameFn getUserName = nullptr) {
  namespace off = nevr_game_offsets;
  PlayerReadResult result;
  const LobbyEntrants lobby = LobbyEntrantsOf(netGame);
  if (ReadAt<const void*>(netGame, off::SESSION_FLAGS_POINTER_OFFSET) == nullptr) getUserName = nullptr;
  uint16_t entrantCount = ReadAt<uint16_t>(netGame, off::PLAYER_COUNT_OFFSET);
  if (entrantCount > off::PLAYER_ARRAY_CAPACITY) entrantCount = off::PLAYER_ARRAY_CAPACITY;

  for (uint16_t i = 0; i < entrantCount; i++) {
    const uint16_t word = ReadAt<uint16_t>(netGame, off::PLAYER_ARRAY_OFFSET + i * off::PLAYER_STRIDE +
                                                        off::PLAYER_FLAGS_OFFSET);
    const uint16_t team = EntrantTeam(word);
    if (team > off::ENTRANT_TEAM_LAST_PLAYER) {
      result.notPlayers++;
      continue;
    }
    const uint8_t slot = EntrantSlot(word);
    if (slot >= kMaxSlots) {
      result.slotOutOfRange++;
      continue;
    }
    if (HandleTableEntrant(netGame, slot) != i) {
      result.notInHandleTable++;
      continue;
    }
    if (slot >= lobby.count) {  // count is 0 when there is no table
      result.noIdentity++;
      continue;
    }

    auto& info = snap->playerInfo[slot];
    info.present = true;
    info.flags = word;
    info.entrantIndex = i;
    info.handleGeneration = HandleTableGeneration(netGame, slot);
    info.teamIndex = team;
    const EchoVR::Lobby::EntrantData& entrant = lobby.items[slot];
    info.accountId = entrant.userId.accountId;
    // The game function takes a mutable net game; it is the caller's own, so dropping const is safe.
    CopyName(info.displayName, getUserName ? getUserName(const_cast<uint8_t*>(netGame), i) : entrant.sfwDisplayName);
    info.ping = entrant.ping;
    info.jerseyNumber = ReadJerseyNumber(netGame, slot);
    info.level = ReadCountStat(netGame, slot, off::SLOT_STAT_LEVEL);
    info.packetLossRatio = ReadPacketLossRatio(netGame, i, slot);
    snap->stats[slot] = ReadPlayerStats(netGame, slot);
    snap->playerCount++;
  }
  return result;
}

}  // namespace nevr_telemetry_player
