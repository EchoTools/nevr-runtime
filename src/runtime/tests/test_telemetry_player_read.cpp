/* SYNTHESIS -- custom tool code, not from binary */

// The telemetry stream's player reads (#2099), against a net game laid out as echovr.exe 34.4.631547.1's
// /session writer reads it. The offsets and strides below are written out from the disassembly, not taken
// from nevr_game_offsets, so a wrong constant in the shipped header fails here:
//   - net-game entrants at +0x178, stride 0x250, count u16 at +0xE2; the first word holds the game slot in
//     bits 0-4 and the team in bits 10-12; a team above 2 is not a player;
//   - the handle table at +0x138: 16 entries { u16 entrant, u16 generation } by game slot; an entrant is read
//     only when its slot's entry names it, and an entry whose entrant or generation is 0xffff names none;
//   - each stat { u32 type, value, u32 count } at netGame + slot * 0x4D08 + off; type 5 is value / count;
//     possession time is a float;
//   - the lobby is *(netGame + 0x40) + 0x18; its entrant table (0xD8 per slot, by game slot) is at +0x360,
//     its length at +0x368; id at +0x08, name at +0x60 (sfwDisplayName), ping at +0x8A. A player whose slot
//     has no entry there is not sent.

#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "runtime/server/telemetry_player_read.h"

namespace {

constexpr uint64_t kEntrants = 0x178;
constexpr uint64_t kEntrantStride = 0x250;
constexpr uint64_t kEntrantCount = 0xE2;
constexpr uint64_t kHandleTable = 0x138;  // { u16 entrant, u16 generation } per slot, 4 bytes each
constexpr uint16_t kNoHandle = 0xFFFF;
constexpr uint64_t kLobbyPointer = 0x40;
constexpr uint64_t kSlotStride = 0x4D08;
constexpr uint64_t kLobbyTable = 0x18 + 0x360;
constexpr uint64_t kLobbyCount = 0x18 + 0x368;
constexpr uint64_t kLobbyEntrantSize = 0xD8;

// /session's stat offsets (0x1401565b2-0x140156785).
constexpr uint64_t kStuns = 0x7184, kBlocks = 0x71b4, kPoints = 0x72a4, kGoals = 0x72d4, kSteals = 0x7394,
                   kAssists = 0x73c4, kSaves = 0x73f4, kShots = 0x7454, kPasses = 0x74b4, kCatches = 0x74e4,
                   kInterceptions = 0x7544, kPossession = 0x75a4;

uint16_t Word(uint8_t slot, uint16_t team) { return static_cast<uint16_t>(slot | (team << 10)); }

class NetGame {
 public:
  // The lobby region is the 0x18 bytes before the CNSLobby and an EchoVR::Lobby, which the reader reads
  // through; its table holds 16 entrants unless a test says otherwise.
  // The handle table starts empty (0xffff); setting an entrant names it in its slot's entry unless an earlier
  // entrant holds that slot, as a game with one record per slot has it.
  NetGame() : bytes_(0x60000, 0), lobby_(0x18 + sizeof(EchoVR::Lobby), 0) {
    SetLobby(16);
    for (uint8_t slot = 0; slot < 16; slot++) SetHandle(slot, kNoHandle, kNoHandle);
  }

  template <typename T>
  void Put(std::vector<uint8_t>& buf, uint64_t off, T value) {
    ASSERT_LE(off + sizeof(T), buf.size());
    std::memcpy(buf.data() + off, &value, sizeof(T));
  }

  void SetEntrants(const std::vector<uint16_t>& words) {
    Put<uint16_t>(bytes_, kEntrantCount, static_cast<uint16_t>(words.size()));
    for (size_t i = 0; i < words.size(); i++) SetEntrantWord(i, words[i]);
  }
  void SetEntrantCount(uint16_t count) { Put<uint16_t>(bytes_, kEntrantCount, count); }
  void SetEntrantWord(size_t index, uint16_t word) {
    Put<uint16_t>(bytes_, kEntrants + index * kEntrantStride, word);
    const uint8_t slot = word & 0x1F;
    if (slot < 16 && HandleEntrant(slot) == kNoHandle) SetHandle(slot, static_cast<uint16_t>(index), 1);
  }
  void SetHandle(uint8_t slot, uint16_t entrant, uint16_t generation) {
    Put<uint16_t>(bytes_, kHandleTable + slot * 4, entrant);
    Put<uint16_t>(bytes_, kHandleTable + slot * 4 + 2, generation);
  }
  uint16_t HandleEntrant(uint8_t slot) const {
    uint16_t entrant;
    std::memcpy(&entrant, bytes_.data() + kHandleTable + slot * 4, sizeof(entrant));
    return entrant;
  }

  void SetStat(uint8_t slot, uint64_t off, uint32_t type, uint32_t value, uint32_t count) {
    const uint64_t at = slot * kSlotStride + off;
    Put<uint32_t>(bytes_, at, type);
    Put<uint32_t>(bytes_, at + 4, value);
    Put<uint32_t>(bytes_, at + 8, count);
  }
  void SetFloatStat(uint8_t slot, uint64_t off, uint32_t type, float value, uint32_t count) {
    const uint64_t at = slot * kSlotStride + off;
    Put<uint32_t>(bytes_, at, type);
    Put<float>(bytes_, at + 4, value);
    Put<uint32_t>(bytes_, at + 8, count);
  }

  // A lobby table whose length says `slots` entrants, each 0xD8 bytes, laid out by byte offset. The memory
  // holds 16, so an entry past the length can carry data a read must not take.
  void SetLobby(uint64_t slots) {
    table_.assign(16 * kLobbyEntrantSize, 0);
    Put<const uint8_t*>(bytes_, kLobbyPointer, lobby_.data());
    Put<const uint8_t*>(lobby_, kLobbyTable, table_.data());
    Put<uint64_t>(lobby_, kLobbyCount, slots);
  }
  void RemoveLobby() { Put<const uint8_t*>(bytes_, kLobbyPointer, nullptr); }
  void SetLobbyEntrant(uint8_t slot, uint64_t accountId, const std::string& name, uint16_t ping) {
    const uint64_t at = slot * kLobbyEntrantSize;
    Put<uint64_t>(table_, at + 0x00, 4);  // platform code; not the id
    Put<uint64_t>(table_, at + 0x08, accountId);
    std::memcpy(table_.data() + at + 0x3C, "not-this-name", 14);  // displayName: /session writes +0x60
    std::memcpy(table_.data() + at + 0x60, name.c_str(), name.size() + 1);
    Put<uint16_t>(table_, at + 0x8A, ping);
    Put<uint16_t>(table_, at + 0x8E, 7);  // the lobby's team word; the team comes from the net game
  }

  const uint8_t* data() const { return bytes_.data(); }
  std::vector<uint8_t> bytes_;
  std::vector<uint8_t> lobby_;
  std::vector<uint8_t> table_;
};

TelemetrySnapshot Read(const NetGame& ng, nevr_telemetry_player::PlayerReadResult* result = nullptr) {
  TelemetrySnapshot snap;
  snap.Clear();
  const auto r = nevr_telemetry_player::ReadPlayers(ng.data(), &snap);
  if (result) *result = r;
  return snap;
}

TEST(TelemetryEntrantWord, SlotIsTheLowFiveBitsAndTeamIsBitsTenToTwelve) {
  EXPECT_EQ(nevr_telemetry_player::EntrantSlot(0x0C05), 5);
  EXPECT_EQ(nevr_telemetry_player::EntrantTeam(0x0C05), 3);
  EXPECT_EQ(nevr_telemetry_player::EntrantSlot(0xFFFF), 31);
  EXPECT_EQ(nevr_telemetry_player::EntrantTeam(0xFFFF), 7);
  EXPECT_EQ(nevr_telemetry_player::EntrantSlot(0x03E0), 0);  // bits 5-9 are neither
  EXPECT_EQ(nevr_telemetry_player::EntrantTeam(0x03E0), 0);
}

TEST(TelemetryRole, ATeamMapsToItsRoleAndAnythingElseToNone) {
  EXPECT_EQ(nevr_telemetry_player::RoleFromTeam(0), telemetry::v2::ROLE_BLUE_TEAM);
  EXPECT_EQ(nevr_telemetry_player::RoleFromTeam(1), telemetry::v2::ROLE_ORANGE_TEAM);
  EXPECT_EQ(nevr_telemetry_player::RoleFromTeam(2), telemetry::v2::ROLE_SPECTATOR);
  EXPECT_EQ(nevr_telemetry_player::RoleFromTeam(3), telemetry::v2::ROLE_UNSPECIFIED);
  EXPECT_EQ(nevr_telemetry_player::RoleFromTeam(4), telemetry::v2::ROLE_UNSPECIFIED);
}

TEST(TelemetryPlayerRead, EachStatIsReadFromItsOwnEntryAtTheSlot) {
  NetGame ng;
  ng.SetEntrants({Word(3, 0)});
  const uint64_t offs[] = {kStuns, kBlocks, kPoints, kGoals, kSteals, kAssists, kSaves, kShots, kPasses, kCatches,
                           kInterceptions};
  for (size_t k = 0; k < sizeof(offs) / sizeof(offs[0]); k++) ng.SetStat(3, offs[k], 1, 101 + k, 9);
  ng.SetFloatStat(3, kPossession, 1, 42.25f, 9);
  // The same entries at slot 0, which no player holds, carry other numbers.
  for (uint64_t off : offs) ng.SetStat(0, off, 1, 999, 9);

  const TelemetrySnapshot snap = Read(ng);
  const auto& s = snap.stats[3];
  EXPECT_EQ(s.stuns, 101u);
  EXPECT_EQ(s.blocks, 102u);
  EXPECT_EQ(s.points, 103u);
  EXPECT_EQ(s.goals, 104u);
  EXPECT_EQ(s.steals, 105u);
  EXPECT_EQ(s.assists, 106u);
  EXPECT_EQ(s.saves, 107u);
  EXPECT_EQ(s.shotsTaken, 108u);
  EXPECT_EQ(s.passes, 109u);
  EXPECT_EQ(s.catches, 110u);
  EXPECT_EQ(s.interceptions, 111u);
  EXPECT_FLOAT_EQ(s.possessionTime, 42.25f);
}

TEST(TelemetryPlayerRead, AnAverageEntryIsValueOverCountAndZeroWithNoCount) {
  NetGame ng;
  ng.SetEntrants({Word(1, 1)});
  ng.SetStat(1, kGoals, 5, 9, 2);    // 9 / 2 = 4, integer division as the game's `div`
  ng.SetStat(1, kPoints, 5, 9, 0);   // no count: 0
  ng.SetStat(1, kStuns, 4, 9, 2);    // not the average type: the value
  ng.SetFloatStat(1, kPossession, 5, 10.0f, 4);

  const TelemetrySnapshot snap = Read(ng);
  EXPECT_EQ(snap.stats[1].goals, 4u);
  EXPECT_EQ(snap.stats[1].points, 0u);
  EXPECT_EQ(snap.stats[1].stuns, 9u);
  EXPECT_FLOAT_EQ(snap.stats[1].possessionTime, 2.5f);

  ng.SetFloatStat(1, kPossession, 5, 10.0f, 0);
  EXPECT_FLOAT_EQ(Read(ng).stats[1].possessionTime, 0.0f);
}

TEST(TelemetryPlayerRead, PlayersAreKeptByGameSlotNotByArrayOrder) {
  NetGame ng;
  ng.SetEntrants({Word(5, 1), Word(2, 0), Word(9, 2)});
  ng.SetStat(5, kStuns, 1, 3, 0);
  ng.SetStat(2, kStuns, 1, 8, 0);

  const TelemetrySnapshot snap = Read(ng);
  EXPECT_EQ(snap.playerCount, 3);
  EXPECT_TRUE(snap.playerInfo[5].present);
  EXPECT_TRUE(snap.playerInfo[2].present);
  EXPECT_TRUE(snap.playerInfo[9].present);
  EXPECT_FALSE(snap.playerInfo[0].present);
  EXPECT_FALSE(snap.playerInfo[1].present);
  EXPECT_EQ(snap.playerInfo[5].teamIndex, 1);
  EXPECT_EQ(snap.playerInfo[2].teamIndex, 0);
  EXPECT_EQ(snap.playerInfo[9].teamIndex, 2);
  EXPECT_EQ(snap.playerInfo[5].flags, Word(5, 1));
  EXPECT_EQ(snap.stats[5].stuns, 3u);
  EXPECT_EQ(snap.stats[2].stuns, 8u);
}

TEST(TelemetryPlayerRead, AnEntrantOfATeamAboveTwoIsNotAPlayer) {
  NetGame ng;
  ng.SetEntrants({Word(0, 4), Word(4, 2), Word(6, 3)});
  nevr_telemetry_player::PlayerReadResult result;
  const TelemetrySnapshot snap = Read(ng, &result);
  EXPECT_FALSE(snap.playerInfo[0].present);
  EXPECT_FALSE(snap.playerInfo[6].present);
  EXPECT_TRUE(snap.playerInfo[4].present);
  EXPECT_EQ(snap.playerCount, 1);
  EXPECT_EQ(result.notPlayers, 2);
  EXPECT_EQ(result.slotOutOfRange, 0);
}

TEST(TelemetryPlayerRead, ASlotPastTheSnapshotIsCountedAndLeftOut) {
  NetGame ng;
  ng.SetEntrants({Word(20, 0), Word(15, 1), Word(16, 1)});
  nevr_telemetry_player::PlayerReadResult result;
  const TelemetrySnapshot snap = Read(ng, &result);
  EXPECT_TRUE(snap.playerInfo[15].present);
  EXPECT_EQ(snap.playerCount, 1);
  EXPECT_EQ(result.slotOutOfRange, 2);
}

TEST(TelemetryPlayerRead, NoMoreThanSixteenEntrantsAreRead) {
  NetGame ng;
  ng.SetEntrantCount(17);
  for (size_t i = 0; i < 16; i++) ng.SetEntrantWord(i, Word(0, 4));  // not players
  ng.SetEntrantWord(16, Word(7, 0));  // a 17th record: past the array
  nevr_telemetry_player::PlayerReadResult result;
  const TelemetrySnapshot snap = Read(ng, &result);
  EXPECT_FALSE(snap.playerInfo[7].present);
  EXPECT_EQ(result.notPlayers, 16);
}

TEST(TelemetryPlayerRead, IdNameAndPingComeFromTheLobbyEntrantAtTheSlot) {
  NetGame ng;
  ng.SetEntrants({Word(5, 1), Word(2, 0)});
  ng.SetLobby(10);
  ng.SetLobbyEntrant(2, 0x0123456789ABCDEFull, "blue_two", 31);
  ng.SetLobbyEntrant(5, 0x00000000DEADBEEFull, "orange_five", 42);
  ng.SetLobbyEntrant(0, 0x1111, "array_index_zero", 99);  // what an array-order read would find

  const TelemetrySnapshot snap = Read(ng);
  EXPECT_EQ(snap.playerInfo[5].accountId, 0x00000000DEADBEEFull);
  EXPECT_STREQ(snap.playerInfo[5].displayName, "orange_five");
  EXPECT_EQ(snap.playerInfo[5].ping, 42);
  EXPECT_EQ(snap.playerInfo[2].accountId, 0x0123456789ABCDEFull);
  EXPECT_STREQ(snap.playerInfo[2].displayName, "blue_two");
  EXPECT_EQ(snap.playerInfo[2].ping, 31);
  EXPECT_EQ(snap.playerInfo[5].teamIndex, 1);  // from the entrant word, not the lobby's +0x8E
}

TEST(TelemetryPlayerRead, ALongNameIsCutAndEnded) {
  NetGame ng;
  ng.SetEntrants({Word(1, 0)});
  ng.SetLobby(4);
  ng.SetLobbyEntrant(1, 7, std::string(35, 'x'), 1);
  ng.Put<char>(ng.table_, 1 * kLobbyEntrantSize + 0x60 + 35, 'y');  // no NUL inside the 36 bytes
  const TelemetrySnapshot snap = Read(ng);
  EXPECT_EQ(std::string(snap.playerInfo[1].displayName), std::string(35, 'x'));
}

TEST(TelemetryPlayerRead, ASlotPastTheLobbyTableIsCountedAndLeftOut) {
  NetGame ng;
  ng.SetEntrants({Word(3, 0), Word(1, 0)});
  ng.SetLobby(3);  // slots 0 to 2; slot 3 is the first one past the length
  ng.SetLobbyEntrant(1, 77, "one", 5);
  ng.SetLobbyEntrant(3, 99, "past_the_length", 6);  // in memory, at the table's length
  ng.SetStat(3, kStuns, 1, 4, 0);
  nevr_telemetry_player::PlayerReadResult result;
  const TelemetrySnapshot snap = Read(ng, &result);
  EXPECT_FALSE(snap.playerInfo[3].present);
  EXPECT_EQ(snap.playerInfo[3].accountId, 0u);
  EXPECT_STREQ(snap.playerInfo[3].displayName, "");
  EXPECT_EQ(snap.stats[3].stuns, 0u);
  EXPECT_TRUE(snap.playerInfo[1].present);
  EXPECT_EQ(snap.playerInfo[1].accountId, 77u);
  EXPECT_EQ(snap.playerCount, 1);
  EXPECT_EQ(result.noIdentity, 1);
  EXPECT_EQ(result.slotOutOfRange, 0);
}

TEST(TelemetryPlayerRead, NoLobbyOrNoTableLeavesEveryPlayerOut) {
  NetGame ng;
  ng.SetEntrants({Word(2, 0)});
  ng.RemoveLobby();  // *(netGame + 0x40) is null
  nevr_telemetry_player::PlayerReadResult result;
  TelemetrySnapshot snap = Read(ng, &result);
  EXPECT_FALSE(snap.playerInfo[2].present);
  EXPECT_EQ(snap.playerCount, 0);
  EXPECT_EQ(result.noIdentity, 1);

  ng.SetLobby(4);
  ng.Put<const uint8_t*>(ng.lobby_, kLobbyTable, nullptr);  // a lobby whose table is not allocated
  snap = Read(ng, &result);
  EXPECT_FALSE(snap.playerInfo[2].present);
  EXPECT_EQ(result.noIdentity, 1);
  EXPECT_EQ(nevr_telemetry_player::LobbyEntrantsOf(ng.data()).count, 0u);
}

TEST(TelemetryPlayerRead, OfTwoEntrantsWithOneSlotTheOneTheHandleTableNamesIsRead) {
  NetGame ng;
  ng.SetEntrants({Word(4, 0), Word(4, 1), Word(6, 2)});
  ng.SetLobbyEntrant(4, 40, "four", 3);
  ng.SetStat(4, kStuns, 1, 2, 0);
  nevr_telemetry_player::PlayerReadResult result;
  TelemetrySnapshot snap = Read(ng, &result);
  EXPECT_TRUE(snap.playerInfo[4].present);
  EXPECT_EQ(snap.playerInfo[4].teamIndex, 0);
  EXPECT_EQ(snap.playerInfo[4].flags, Word(4, 0));
  EXPECT_EQ(snap.playerInfo[4].accountId, 40u);
  EXPECT_TRUE(snap.playerInfo[6].present);
  EXPECT_EQ(snap.playerCount, 2);
  EXPECT_EQ(result.notInHandleTable, 1);

  // The table names the second record: it is the one read, whatever its place in the array.
  ng.SetHandle(4, 1, 1);
  snap = Read(ng, &result);
  EXPECT_TRUE(snap.playerInfo[4].present);
  EXPECT_EQ(snap.playerInfo[4].teamIndex, 1);
  EXPECT_EQ(snap.playerInfo[4].flags, Word(4, 1));
  EXPECT_EQ(snap.playerInfo[4].accountId, 40u);
  EXPECT_EQ(snap.stats[4].stuns, 2u);
  EXPECT_EQ(snap.playerCount, 2);
  EXPECT_EQ(result.notInHandleTable, 1);
}

TEST(TelemetryPlayerRead, AnEntrantWhoseSlotEntryIsEmptyIsCountedAndLeftOut) {
  NetGame ng;
  ng.SetEntrants({Word(2, 0), Word(5, 1), Word(7, 0)});
  ng.SetLobbyEntrant(2, 20, "two", 1);
  ng.SetLobbyEntrant(5, 50, "five", 1);
  ng.SetLobbyEntrant(7, 70, "seven", 1);
  ng.SetHandle(2, kNoHandle, 1);   // no entrant
  ng.SetHandle(5, 1, kNoHandle);   // names entrant 1, but the generation says the handle is invalid
  ng.SetStat(2, kStuns, 1, 3, 0);
  nevr_telemetry_player::PlayerReadResult result;
  const TelemetrySnapshot snap = Read(ng, &result);
  EXPECT_FALSE(snap.playerInfo[2].present);
  EXPECT_EQ(snap.playerInfo[2].accountId, 0u);
  EXPECT_EQ(snap.stats[2].stuns, 0u);
  EXPECT_FALSE(snap.playerInfo[5].present);
  EXPECT_TRUE(snap.playerInfo[7].present);
  EXPECT_EQ(snap.playerInfo[7].accountId, 70u);
  EXPECT_EQ(snap.playerCount, 1);
  EXPECT_EQ(result.notInHandleTable, 2);
  EXPECT_EQ(result.noIdentity, 0);
  EXPECT_EQ(result.slotOutOfRange, 0);
}

TEST(TelemetryPlayerRead, AnEntrantTheTableDoesNotNameIsLeftOutEvenWithItsSlotFree) {
  NetGame ng;
  ng.SetEntrants({Word(3, 0)});
  ng.SetHandle(3, 9, 1);  // entry 3 names entrant 9, a record past the count
  nevr_telemetry_player::PlayerReadResult result;
  const TelemetrySnapshot snap = Read(ng, &result);
  EXPECT_FALSE(snap.playerInfo[3].present);
  EXPECT_EQ(snap.playerCount, 0);
  EXPECT_EQ(result.notInHandleTable, 1);
}

}  // namespace
