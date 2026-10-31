// The LogInSuccess bit 9 gate (patch/login_success_gate.cpp). The decision is pure and tested directly. The hook is
// driven under MinHook on a stand-in with the function's signature: the stand-in records the flags word it sees, so
// the test observes what the original would have read. Without the hook the stand-in sees bit 9 clear.

#include <gtest/gtest.h>

#include <windows.h>

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "core/hooking.h"
#include "core/logging.h"
#include "runtime/patch/login_success_gate.h"
#include "runtime/patch/login_success_gate_rules.h"

namespace {

using nevr_login_success_gate::kSocialCommunityBit;

// The stand-in for CR15NetGame::LogInSuccess: it reads the flags word through [self+0x2DA0] the way the stock bit test
// does, and records it.
volatile std::uint64_t g_wordSeen = 0;
volatile int g_standInCalls = 0;

__attribute__((noinline)) void __fastcall StandIn(void* self) {
  std::uint64_t* flags = nullptr;
  std::memcpy(&flags, static_cast<std::uint8_t*>(self) + 0x2DA0, sizeof(flags));
  g_wordSeen = *flags;
  g_standInCalls = g_standInCalls + 1;
}

std::vector<std::string> g_logged;
void CaptureLog(EchoVR::LogLevel, const CHAR* message) { g_logged.push_back(message); }

// A netgame object: the state word at +0, the flags pointer at +0x2DA0.
std::vector<std::uint8_t> MakeNetGame(std::uint64_t* word) {
  std::vector<std::uint8_t> netgame(0x2DA8, 0);
  std::memcpy(netgame.data() + 0x2DA0, &word, sizeof(word));
  return netgame;
}

}  // namespace

TEST(LoginSuccessGateRules, ForcesTheSocialBitOnlyWhenItIsClear) {
  EXPECT_EQ(kSocialCommunityBit, 0x200u);
  EXPECT_EQ(nevr_login_success_gate::BitsToForce(0), kSocialCommunityBit);
  EXPECT_EQ(nevr_login_success_gate::BitsToForce(0x5 | (std::uint64_t{1} << 20)), kSocialCommunityBit);
  EXPECT_EQ(nevr_login_success_gate::BitsToForce(kSocialCommunityBit), 0u);
  EXPECT_EQ(nevr_login_success_gate::BitsToForce(kSocialCommunityBit | 0x5), 0u);
}

class LoginSuccessGateHook : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_TRUE(Hooking::Initialize());
    SetBootLogSink(&CaptureLog);
    g_logged.clear();
    // The detour is installed once for the process; every test calls the same stand-in through it.
    static const bool attached =
        nevr_login_success_gate::AttachTo(reinterpret_cast<void*>(&StandIn), "LogInSuccess (test stand-in)");
    ASSERT_TRUE(attached);
  }
};

TEST_F(LoginSuccessGateHook, SetsBit9DuringTheCallAndRestoresTheWord) {
  std::uint64_t word = 0x5;  // bits 0 and 2; bit 9 clear
  std::vector<std::uint8_t> netgame = MakeNetGame(&word);
  StandIn(netgame.data());
  EXPECT_NE(g_wordSeen & kSocialCommunityBit, 0u) << "the original must see bit 9 forced";
  EXPECT_EQ(word, 0x5u) << "the word must be restored: no bit the hook set may remain";
}

TEST_F(LoginSuccessGateHook, LeavesABitThatWasAlreadySetSet) {
  std::uint64_t word = kSocialCommunityBit | 0x5;
  std::vector<std::uint8_t> netgame = MakeNetGame(&word);
  StandIn(netgame.data());
  EXPECT_NE(g_wordSeen & kSocialCommunityBit, 0u);
  EXPECT_EQ(word, kSocialCommunityBit | 0x5u) << "a bit the hook did not set must not be cleared";
}

TEST_F(LoginSuccessGateHook, LogsOneRecordPerLoginWithWhetherTheBitWasSet) {
  std::uint64_t word = 0x5;
  std::vector<std::uint8_t> netgame = MakeNetGame(&word);
  StandIn(netgame.data());
  bool found = false;
  for (const std::string& line : g_logged) {
    if (line.find("[NEVR.PATCH] login success bit9 forced was_set=false state_before=0") != std::string::npos) {
      found = true;
    }
  }
  EXPECT_TRUE(found) << "the per-login record is missing; captured " << g_logged.size() << " lines";
}
