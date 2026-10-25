// Tests for the #403 self-check (compat/party_leave_check.h): each SNSPartyLeaveNotify the game client receives
// becomes one self-check result. The frames the unit sends are read back
// with the small decoder below (the game service's reader, sharing no code with the codec).

#include "runtime/compat/evr_codec.h"
#include "runtime/compat/party_leave_check.h"
#include "runtime/compat/self_check.h"
#include "runtime/compat/social_party.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace {

uint64_t U64(const std::string& s, std::size_t at) {
  uint64_t v = 0;
  for (std::size_t i = 0; i < 8; ++i) v |= static_cast<uint64_t>(static_cast<unsigned char>(s.at(at + i))) << (8 * i);
  return v;
}
uint32_t U32(const std::string& s, std::size_t at) {
  uint32_t v = 0;
  for (std::size_t i = 0; i < 4; ++i) v |= static_cast<uint32_t>(static_cast<unsigned char>(s.at(at + i))) << (8 * i);
  return v;
}

// The strings of one SNSRemoteLogSetv3 frame (nakama RemoteLogSet.Stream + StreamStringTable).
std::vector<std::string> Strings(const std::string& frame) {
  std::vector<std::string> out;
  const std::size_t base = 24;
  std::size_t at = base + 8 * 7;
  const uint64_t count = U64(frame, at);
  at += 8;
  std::vector<uint32_t> offsets(static_cast<std::size_t>(count), 0);
  for (std::size_t i = 1; i < count; ++i) {
    offsets[i] = U32(frame, at);
    at += 4;
  }
  for (std::size_t i = 0; i < count; ++i) {
    std::string s;
    for (std::size_t p = at + offsets[i]; p < frame.size() && frame[p] != '\0'; ++p) s.push_back(frame[p]);
    out.push_back(std::move(s));
  }
  return out;
}

std::vector<std::string> g_frames;
bool Sender(const std::string& frame) {
  g_frames.push_back(frame);
  return true;
}

std::vector<nlohmann::json> Results() {
  std::vector<nlohmann::json> out;
  for (const std::string& f : g_frames) {
    for (const std::string& s : Strings(f)) out.push_back(nlohmann::json::parse(s));
  }
  return out;
}

class PartyLeaveCheck : public ::testing::Test {
 protected:
  void SetUp() override {
    nevr_self_check::ResetForTest();
    nevr_party_leave_check::ResetForTest();
    g_frames.clear();
    nevr_self_check::SetEnabled(true);
    nevr_self_check::SetSender(&Sender);
    nevr_self_check::SetLoggedIn(true, nevr_evr_codec::UserId{4, 77});
  }
  static uint64_t LeaveSymbol() { return nevr_social_party::ReplySymbol("PartyLeaveNotify"); }
  // SNSPartyLeaveNotify: PartyID(8) MemberID(8), little endian.
  static void Notify(uint64_t party, uint64_t member, std::size_t length = 16) {
    std::uint8_t payload[16] = {};
    for (int i = 0; i < 8; ++i) {
      payload[i] = static_cast<std::uint8_t>(party >> (8 * i));
      payload[8 + i] = static_cast<std::uint8_t>(member >> (8 * i));
    }
    nevr_party_leave_check::OnServerMessage(LeaveSymbol(), payload, length);
  }
  static std::vector<nlohmann::json> Leaves() {
    nevr_self_check::Flush();
    std::vector<nlohmann::json> out;
    for (const auto& r : Results()) {
      if (r["check"] == "party_member_leave") out.push_back(r);
    }
    return out;
  }
};

}  // namespace

TEST_F(PartyLeaveCheck, ALeaveNotifyIsOnePassingResultNamingPartyAndMember) {
  Notify(7, 9);
  const auto r = Leaves();
  ASSERT_EQ(r.size(), 1u);
  EXPECT_EQ(r[0]["pass"], true);
  EXPECT_EQ(r[0]["observed"], "leave_notify party=7 member=9 notifies=1");
}

TEST_F(PartyLeaveCheck, EachNotifyIsCountedWithinTheSession) {
  Notify(7, 9);
  Notify(7, 10);
  const auto r = Leaves();
  ASSERT_EQ(r.size(), 2u);
  EXPECT_EQ(r[1]["observed"], "leave_notify party=7 member=10 notifies=2");
}

TEST_F(PartyLeaveCheck, ANotifyForPartyZeroFails) {
  // The regression nakama#681 removed: a LeaveNotify{PartyID:0} reaching a party-group peer.
  Notify(0, 9);
  const auto r = Leaves();
  ASSERT_EQ(r.size(), 1u);
  EXPECT_EQ(r[0]["pass"], false);
  EXPECT_EQ(r[0]["observed"], "leave_notify party=0 member=9 notifies=1 malformed");
}

TEST_F(PartyLeaveCheck, ANotifyNamingNoMemberFails) {
  Notify(7, 0);
  const auto r = Leaves();
  ASSERT_EQ(r.size(), 1u);
  EXPECT_EQ(r[0]["pass"], false);
}

TEST_F(PartyLeaveCheck, AShortNotifyFails) {
  Notify(7, 9, 15);
  const auto r = Leaves();
  ASSERT_EQ(r.size(), 1u);
  EXPECT_EQ(r[0]["pass"], false);
  EXPECT_EQ(r[0]["observed"], "leave_notify short length=15 notifies=1 malformed");
}

TEST_F(PartyLeaveCheck, OtherSymbolsAreIgnored) {
  std::uint8_t payload[16] = {};
  nevr_party_leave_check::OnServerMessage(nevr_social_party::ReplySymbol("PartyJoinNotify"), payload, sizeof(payload));
  EXPECT_TRUE(Leaves().empty());
}

TEST_F(PartyLeaveCheck, ASessionBoundaryStartsTheCountOver) {
  Notify(7, 9);
  nevr_self_check::SetLoggedIn(false);
  nevr_self_check::SetLoggedIn(true, nevr_evr_codec::UserId{4, 78});
  Notify(8, 9);
  const auto r = Leaves();
  ASSERT_EQ(r.size(), 2u);
  EXPECT_EQ(r[1]["observed"], "leave_notify party=8 member=9 notifies=1");
}
