// Tests for the #405 self-check (compat/friend_request_check.h): a friend request that arrives, repeats or ends
// becomes one self-check result. The frames the unit sends are read back
// with the small decoder below (the game service's reader, sharing no code with the codec).

#include "runtime/compat/evr_codec.h"
#include "runtime/compat/friend_request_check.h"
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

class FriendRequestCheck : public ::testing::Test {
 protected:
  static constexpr uint64_t kInviteNotify = 0xca09b0b36bd981b7ULL;   // SNSFriendInviteNotify
  static constexpr uint64_t kAcceptSuccess = 0x1bbda7fa06af4627ULL;  // SNSFriendAcceptSuccess

  void SetUp() override {
    nevr_self_check::ResetForTest();
    nevr_friend_request_check::ResetForTest();
    g_frames.clear();
    nevr_self_check::SetEnabled(true);
    nevr_self_check::SetSender(&Sender);
    nevr_self_check::SetLoggedIn(true, nevr_evr_codec::UserId{4, 77});
  }
  // Header(8) FriendID(8): the other account is the second u64 of every friend message.
  static void Message(uint64_t symbol, uint64_t account, std::size_t length = 16) {
    std::uint8_t payload[16] = {};
    for (int i = 0; i < 8; ++i) payload[8 + i] = static_cast<std::uint8_t>(account >> (8 * i));
    nevr_friend_request_check::OnServerMessage(symbol, payload, length);
  }
  static std::vector<nlohmann::json> Requests() {
    nevr_self_check::Flush();
    std::vector<nlohmann::json> out;
    for (const auto& r : Results()) {
      if (r["check"] == "friend_request") out.push_back(r);
    }
    return out;
  }
};

}  // namespace

TEST_F(FriendRequestCheck, AnIncomingRequestIsOnePassingResult) {
  Message(kInviteNotify, 7);
  const auto r = Requests();
  ASSERT_EQ(r.size(), 1u);
  EXPECT_EQ(r[0]["pass"], true);
  EXPECT_EQ(r[0]["observed"], "request received account=7 pending=1 received=1 cleared=0 duplicate=0");
}

TEST_F(FriendRequestCheck, ARepeatedInviteForAPendingRequestIsCountedAsADuplicate) {
  Message(kInviteNotify, 7);
  Message(kInviteNotify, 7);
  const auto r = Requests();
  ASSERT_EQ(r.size(), 2u);
  EXPECT_EQ(r[1]["pass"], true);
  EXPECT_EQ(r[1]["observed"], "request duplicate account=7 pending=1 received=1 cleared=0 duplicate=1");
}

TEST_F(FriendRequestCheck, AnAcceptEndsTheRequestAndNamesTheMessage) {
  Message(kInviteNotify, 7);
  Message(kAcceptSuccess, 7);
  const auto r = Requests();
  ASSERT_EQ(r.size(), 2u);
  EXPECT_EQ(r[1]["pass"], true);
  EXPECT_EQ(r[1]["observed"],
            "request cleared account=7 reason=FriendAcceptSuccess pending=0 received=1 cleared=1 duplicate=0");
}

TEST_F(FriendRequestCheck, AnEndForARequestNeverSeenReportsNothing) {
  Message(kAcceptSuccess, 9);
  EXPECT_TRUE(Requests().empty());
}

TEST_F(FriendRequestCheck, ALoginStartsTheListAndTheCountsOver) {
  Message(kInviteNotify, 7);
  Message(nevr_evr_codec::kSymLoginSuccess, 0);
  Message(kInviteNotify, 7);
  const auto r = Requests();
  ASSERT_EQ(r.size(), 2u);
  EXPECT_EQ(r[1]["observed"], "request received account=7 pending=1 received=1 cleared=0 duplicate=0");
}

TEST_F(FriendRequestCheck, AnInviteNamingNoAccountFails) {
  Message(kInviteNotify, 0);
  const auto r = Requests();
  ASSERT_EQ(r.size(), 1u);
  EXPECT_EQ(r[0]["pass"], false);
  EXPECT_EQ(r[0]["observed"], "request invite unreadable length=16 malformed");
}

TEST_F(FriendRequestCheck, AShortInviteFails) {
  Message(kInviteNotify, 7, 15);
  const auto r = Requests();
  ASSERT_EQ(r.size(), 1u);
  EXPECT_EQ(r[0]["pass"], false);
  EXPECT_EQ(r[0]["observed"], "request invite unreadable length=15 malformed");
}

TEST_F(FriendRequestCheck, OtherSymbolsAreIgnored) {
  Message(0x1234567890abcdefULL, 7);
  EXPECT_TRUE(Requests().empty());
}

TEST_F(FriendRequestCheck, ASessionBoundaryStartsTheCountsOver) {
  Message(kInviteNotify, 7);
  nevr_self_check::SetLoggedIn(false);
  nevr_self_check::SetLoggedIn(true, nevr_evr_codec::UserId{4, 78});
  Message(kInviteNotify, 8);
  const auto r = Requests();
  ASSERT_EQ(r.size(), 2u);
  EXPECT_EQ(r[1]["observed"], "request received account=8 pending=1 received=1 cleared=0 duplicate=0");
}
