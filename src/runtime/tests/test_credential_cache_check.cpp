// Tests for the #562 self-check (runtime/compat/credential_cache_check.h): what the credential cache did in this
// process (core/credential_cache_stats.h) becomes one self-check result per change, the refusal first.

#include "runtime/compat/credential_cache_check.h"
#include "runtime/compat/evr_codec.h"
#include "runtime/compat/self_check.h"
#include "core/credential_cache_stats.h"

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
std::vector<std::string> Strings(const std::string& frame) {
  std::vector<std::string> out;
  std::size_t at = 24 + 8 * 7;
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

class CredentialCacheCheck : public ::testing::Test {
 protected:
  void SetUp() override {
    nevr_self_check::ResetForTest();
    nevr_credential_stats::ResetForTest();
    nevr_credential_cache_check::ResetForTest();
    g_frames.clear();
    nevr_self_check::SetEnabled(true);
    nevr_self_check::SetSender(&Sender);
    nevr_self_check::SetLoggedIn(true, nevr_evr_codec::UserId{4, 77});
    nevr_credential_cache_check::Register(&nevr_credential_cache_check::ProcessProbe);
  }
  static std::vector<nlohmann::json> Results() {
    nevr_self_check::Flush();
    std::vector<nlohmann::json> out;
    for (const std::string& f : g_frames) {
      for (const std::string& s : Strings(f)) {
        const nlohmann::json j = nlohmann::json::parse(s);
        if (j["check"] == "credential_cache") out.push_back(j);
      }
    }
    return out;
  }
};

}  // namespace

TEST_F(CredentialCacheCheck, NothingToSayWhenTheCacheDidNothing) { EXPECT_TRUE(Results().empty()); }

TEST_F(CredentialCacheCheck, AWrittenCacheIsOnePassingResultNamingTheProtection) {
  nevr_credential_stats::NoteSaved("host_mode_600");
  const auto r = Results();
  ASSERT_EQ(r.size(), 1u);
  EXPECT_EQ(r[0]["pass"], true);
  EXPECT_EQ(r[0]["observed"], "save ok protection=host_mode_600 saves=1 refused=0");
}

TEST_F(CredentialCacheCheck, ARefusedSaveFailsAndNamesTheReason) {
  nevr_credential_stats::NoteRefused("host mode is 644, not 600");
  const auto r = Results();
  ASSERT_EQ(r.size(), 1u);
  EXPECT_EQ(r[0]["pass"], false);
  EXPECT_EQ(r[0]["observed"], "save refused why=host mode is 644, not 600 saves=0 refused=1");
}

TEST_F(CredentialCacheCheck, ReusedCredentialsPass) {
  nevr_credential_stats::NoteCacheReused();
  const auto r = Results();
  ASSERT_EQ(r.size(), 1u);
  EXPECT_EQ(r[0]["pass"], true);
  EXPECT_EQ(r[0]["observed"], "cache reused no_sign_in_needed");
}

TEST_F(CredentialCacheCheck, ASignInThatWasNeededIsReportedAndPasses) {
  nevr_credential_stats::NoteSignInRequired();
  const auto r = Results();
  ASSERT_EQ(r.size(), 1u);
  EXPECT_EQ(r[0]["pass"], true);
  EXPECT_EQ(r[0]["observed"], "cache not reused sign_in_required");
}

TEST_F(CredentialCacheCheck, EachChangeIsReportedOnce) {
  nevr_credential_stats::NoteSaved("host_mode_600");
  EXPECT_EQ(Results().size(), 1u);
  EXPECT_EQ(Results().size(), 1u) << "nothing new, so nothing more";
  nevr_credential_stats::NoteSaved("host_mode_600");
  const auto r = Results();
  ASSERT_EQ(r.size(), 2u);
  EXPECT_EQ(r[1]["observed"], "save ok protection=host_mode_600 saves=2 refused=0");
}

TEST_F(CredentialCacheCheck, ARefusalIsSaidBeforeASaveWhenBothArePending) {
  nevr_credential_stats::NoteSaved("host_mode_600");
  nevr_credential_stats::NoteRefused("cannot start /bin/sh through Wine: 2");
  const auto r = Results();  // one Flush: one result per probe call, the next Flush says the rest
  ASSERT_GE(r.size(), 1u);
  EXPECT_EQ(r[0]["pass"], false);
  EXPECT_EQ(Results().size(), 2u) << "the save follows on the next flush";
}
