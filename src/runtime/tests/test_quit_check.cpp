// Tests for the #392 self-check (lifecycle/quit_check.h): the client's game loop returning becomes one
// self-check result, sent before the process goes. The frames the unit sends are read back
// with the small decoder below (the game service's reader, sharing no code with the codec).

#include "runtime/compat/evr_codec.h"
#include "runtime/lifecycle/quit_check.h"
#include "runtime/compat/self_check.h"

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

class QuitCheck : public ::testing::Test {
 protected:
  void SetUp() override {
    nevr_self_check::ResetForTest();
    nevr_quit_check::ResetForTest();
    g_frames.clear();
    nevr_self_check::SetEnabled(true);
    nevr_self_check::SetSender(&Sender);
  }
  // Not Flush(): the check must send by itself, the process is about to exit.
  static std::vector<nlohmann::json> Quits() {
    std::vector<nlohmann::json> out;
    for (const auto& r : Results()) {
      if (r["check"] == "pc_quit") out.push_back(r);
    }
    return out;
  }
};

}  // namespace

TEST_F(QuitCheck, ACleanReturnIsOnePassingResultSentAtOnce) {
  nevr_self_check::SetLoggedIn(true, nevr_evr_codec::UserId{4, 77});
  nevr_quit_check::OnGameLoopReturned(0);
  ASSERT_EQ(g_frames.size(), 1u) << "the result left without a Flush";
  const auto r = Quits();
  ASSERT_EQ(r.size(), 1u);
  EXPECT_EQ(r[0]["pass"], true);
  EXPECT_EQ(r[0]["observed"], "game_loop_returned exit_code=0");
}

TEST_F(QuitCheck, ANonZeroExitCodeFails) {
  nevr_self_check::SetLoggedIn(true, nevr_evr_codec::UserId{4, 77});
  nevr_quit_check::OnGameLoopReturned(3);
  const auto r = Quits();
  ASSERT_EQ(r.size(), 1u);
  EXPECT_EQ(r[0]["pass"], false);
  EXPECT_EQ(r[0]["observed"], "game_loop_returned exit_code=3");
}

TEST_F(QuitCheck, WithoutALoginTheResultIsLoggedButNoFrameIsSent) {
  nevr_quit_check::OnGameLoopReturned(0);
  EXPECT_TRUE(g_frames.empty());
}

TEST_F(QuitCheck, ASecondReturnReportsAgain) {
  nevr_self_check::SetLoggedIn(true, nevr_evr_codec::UserId{4, 77});
  nevr_quit_check::OnGameLoopReturned(0);
  nevr_quit_check::OnGameLoopReturned(0);
  EXPECT_EQ(Quits().size(), 2u);
}
