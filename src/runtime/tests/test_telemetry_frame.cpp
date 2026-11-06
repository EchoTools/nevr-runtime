/* SYNTHESIS -- custom tool code, not from binary */

// The stream's capture header and frames built from a snapshot, and the session values the snapshot takes from
// the game (#2100), against a net game laid out as echovr.exe 34.4.631547.1's /session writer (0x1401b2ea0)
// reads it. The offsets and keys below are written out from the disassembly, not taken from the shipped
// headers:
//   - map_name is the `const char*` at *(netGame + 0x40) + 0x470 (0x1401b30bd, 0x1401b30c6), "" when the
//     object at +0x40 is null;
//   - total_round_count is the int getter (0x140d05730) on the net game with the CSymbol64 of
//     "net_total_round_count" (0x1401b31c0-0x1401b31d2).

#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "nevr/symbol_hash.h"
#include "runtime/server/telemetry_frame.h"
#include "runtime/server/telemetry_session_read.h"

namespace {

constexpr uint64_t kSessionObject = 0x40;
constexpr uint64_t kLevelName = 0x470;

template <typename T>
void Put(std::vector<uint8_t>& buf, uint64_t off, T value) {
  ASSERT_LE(off + sizeof(T), buf.size());
  std::memcpy(buf.data() + off, &value, sizeof(T));
}

// A net game with the object at +0x40 whose +0x470 names the level.
struct Session {
  Session() : netGame(0x3000, 0), object(0x500, 0) {
    Put<const uint8_t*>(netGame, kSessionObject, object.data());
  }
  void SetLevel(const char* name) { Put<const char*>(object, kLevelName, name); }
  void* ng() { return netGame.data(); }
  std::vector<uint8_t> netGame;
  std::vector<uint8_t> object;
};

struct IntCall {
  void* owner;
  uint64_t key;
};
std::vector<IntCall> g_intCalls;

int32_t __fastcall FakeInt(void* owner, uint64_t key) {
  g_intCalls.push_back({owner, key});
  return key == EchoVR::CSymbol64Hash("net_total_round_count") ? 1 : -99;
}

TelemetrySnapshot Cleared() {
  TelemetrySnapshot s;
  s.Clear();
  return s;
}

TEST(TelemetrySessionKeys, EachKeyIsTheCSymbol64OfItsName) {
  EXPECT_EQ(nevr_property_hash::TOTAL_ROUND_COUNT, EchoVR::CSymbol64Hash("net_total_round_count"));
}

TEST(TelemetrySessionRead, TheLevelIsTheNameTheSessionObjectPointsTo) {
  Session s;
  s.SetLevel("mpl_combat_dyson");
  TelemetrySnapshot snap = Cleared();
  nevr_telemetry_session::ReadSessionState({nullptr}, s.ng(), &snap);
  EXPECT_STREQ(snap.mapName, "mpl_combat_dyson");
}

TEST(TelemetrySessionRead, NoSessionObjectOrNoNameIsAnEmptyLevel) {
  Session s;
  TelemetrySnapshot snap = Cleared();
  std::memcpy(snap.mapName, "stale", 6);
  nevr_telemetry_session::ReadSessionState({nullptr}, s.ng(), &snap);  // the name pointer is null
  EXPECT_STREQ(snap.mapName, "");

  s.SetLevel("mpl_arena_a");
  Put<const uint8_t*>(s.netGame, kSessionObject, nullptr);
  std::memcpy(snap.mapName, "stale", 6);
  nevr_telemetry_session::ReadSessionState({nullptr}, s.ng(), &snap);
  EXPECT_STREQ(snap.mapName, "");
}

TEST(TelemetrySessionRead, ALongLevelNameIsCutAndEnded) {
  Session s;
  const std::string longName(100, 'm');
  s.SetLevel(longName.c_str());
  TelemetrySnapshot snap = Cleared();
  nevr_telemetry_session::ReadSessionState({nullptr}, s.ng(), &snap);
  EXPECT_EQ(std::string(snap.mapName), std::string(63, 'm'));
}

TEST(TelemetrySessionRead, TheRoundCountIsTheGamesOwnOnTheNetGame) {
  Session s;
  g_intCalls.clear();
  TelemetrySnapshot snap = Cleared();
  nevr_telemetry_session::ReadSessionState({FakeInt}, s.ng(), &snap);
  EXPECT_EQ(snap.totalRoundCount, 1);
  bool asked = false;
  for (const auto& c : g_intCalls) {
    EXPECT_EQ(c.owner, s.ng());
    asked = asked || c.key == EchoVR::CSymbol64Hash("net_total_round_count");
  }
  EXPECT_TRUE(asked);

  TelemetrySnapshot none = Cleared();
  nevr_telemetry_session::ReadSessionState({nullptr}, s.ng(), &none);
  EXPECT_EQ(none.totalRoundCount, 0);
}

TEST(TelemetryHeader, TheMapAndRoundCountAreTheSessionsNotConstants) {
  TelemetrySnapshot snap = Cleared();
  std::memcpy(snap.mapName, "mpl_combat_dyson", 17);
  snap.totalRoundCount = 1;
  telemetry::v2::EchoArenaHeader ah;
  nevr_telemetry_frame::BuildArenaHeader(snap, "SESSION-1", &ah);
  EXPECT_EQ(ah.map_name(), "mpl_combat_dyson");
  EXPECT_EQ(ah.total_round_count(), 1);
  EXPECT_EQ(ah.session_id(), "SESSION-1");

  std::memcpy(snap.mapName, "mpl_arena_a", 12);
  snap.totalRoundCount = 5;
  telemetry::v2::EchoArenaHeader again;
  nevr_telemetry_frame::BuildArenaHeader(snap, "SESSION-1", &again);
  EXPECT_EQ(again.map_name(), "mpl_arena_a");
  EXPECT_EQ(again.total_round_count(), 5);
}

TEST(TelemetryHeader, TheRosterHoldsEachPresentSlotWithItsRole) {
  TelemetrySnapshot snap = Cleared();
  snap.playerInfo[4].present = true;
  snap.playerInfo[4].accountId = 77;
  snap.playerInfo[4].teamIndex = 1;
  std::memcpy(snap.playerInfo[4].displayName, "orange4", 8);
  snap.playerInfo[9].accountId = 99;  // not present
  telemetry::v2::EchoArenaHeader ah;
  nevr_telemetry_frame::BuildArenaHeader(snap, "S", &ah);
  ASSERT_EQ(ah.initial_roster_size(), 1);
  EXPECT_EQ(ah.initial_roster(0).slot(), 4);
  EXPECT_EQ(ah.initial_roster(0).account_number(), 77u);
  EXPECT_EQ(ah.initial_roster(0).display_name(), "orange4");
  EXPECT_EQ(ah.initial_roster(0).role(), telemetry::v2::ROLE_ORANGE_TEAM);
}

}  // namespace
