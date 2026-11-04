#pragma once

#include <cstdint>
#include <cstring>

#include "abi/echovr.h"
#include "runtime/server/telemetry_snapshot.h"

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

// What ReadPlayers left out: entrants of a team above 2 (the server's own entrant is team 4), as /session
// does; and players it could not send: a slot at or past kMaxSlots, a slot with no entry in the lobby's
// entrant table (no account id or name), and a slot an earlier entrant already holds.
struct PlayerReadResult {
  uint16_t notPlayers = 0;
  uint16_t slotOutOfRange = 0;
  uint16_t noIdentity = 0;
  uint16_t duplicateSlot = 0;
};

// Fills a cleared snapshot's playerInfo and stats by game slot (an absent slot keeps present == false) and
// counts the players read in snap->playerCount. The identity (account id, name, ping) is the lobby entrant at
// the same slot; a player whose slot is past the lobby's table is left out, and of two entrants with one slot
// the first is kept.
inline PlayerReadResult ReadPlayers(const uint8_t* netGame, TelemetrySnapshot* snap) {
  namespace off = nevr_game_offsets;
  PlayerReadResult result;
  const LobbyEntrants lobby = LobbyEntrantsOf(netGame);
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
    if (slot >= lobby.count) {  // count is 0 when there is no table
      result.noIdentity++;
      continue;
    }

    auto& info = snap->playerInfo[slot];
    if (info.present) {
      result.duplicateSlot++;
      continue;
    }
    info.present = true;
    info.flags = word;
    info.teamIndex = team;
    const EchoVR::Lobby::EntrantData& entrant = lobby.items[slot];
    info.accountId = entrant.userId.accountId;
    std::memcpy(info.displayName, entrant.sfwDisplayName, sizeof(info.displayName));
    info.displayName[sizeof(info.displayName) - 1] = '\0';
    info.ping = entrant.ping;
    snap->stats[slot] = ReadPlayerStats(netGame, slot);
    snap->playerCount++;
  }
  return result;
}

}  // namespace nevr_telemetry_player
