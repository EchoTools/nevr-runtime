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
    nevr_credential_cache_check::Register();
  }
  // Flushes, then every result of one check sent so far.
  static std::vector<nlohmann::json> Results(const char* check = "credential_cache") {
    nevr_self_check::Flush();
    std::vector<nlohmann::json> out;
    for (const std::string& f : g_frames) {
      for (const std::string& s : Strings(f)) {
        const nlohmann::json j = nlohmann::json::parse(s);
        if (j["check"] == check) out.push_back(j);
      }
    }
    return out;
  }
};

}  // namespace

using nevr_credential_stats::SignInReason;

TEST_F(CredentialCacheCheck, NothingToSayWhenTheCacheDidNothing) {
  EXPECT_TRUE(Results().empty());
  EXPECT_TRUE(Results("credential_cache_refused").empty());
}

TEST_F(CredentialCacheCheck, AWrittenCacheIsOnePassingResultNamingTheProtection) {
  nevr_credential_stats::NoteSaved("host_mode_600");
  const auto r = Results();
  ASSERT_EQ(r.size(), 1u);
  EXPECT_EQ(r[0]["pass"], true);
  EXPECT_EQ(r[0]["observed"], "save ok protection=host_mode_600 saves=1 refused=0");
}

// L1 of the #562 review: a refusal is its own check, so saves and reuse can never use up its per-check cap.
TEST_F(CredentialCacheCheck, ARefusedSaveFailsInItsOwnCheckAndNamesTheReason) {
  nevr_credential_stats::NoteRefused("host mode is 644, not 600");
  EXPECT_TRUE(Results().empty()) << "a refusal is not a result of the saves check";
  const auto r = Results("credential_cache_refused");
  ASSERT_EQ(r.size(), 1u);
  EXPECT_EQ(r[0]["pass"], false);
  EXPECT_EQ(r[0]["observed"], "save refused why=host mode is 644, not 600 saves=0 refused=1");
}

// L1: a long session saves about six times (every refresh); only the first save and a change of protection are said.
TEST_F(CredentialCacheCheck, ManySavesWithTheSameProtectionAreSaidOnce) {
  for (int i = 0; i < 10; ++i) {
    nevr_credential_stats::NoteSaved("host_mode_600");
    (void)Results();
  }
  const auto r = Results();
  ASSERT_EQ(r.size(), 1u);
  EXPECT_EQ(r[0]["observed"], "save ok protection=host_mode_600 saves=1 refused=0");
}

TEST_F(CredentialCacheCheck, AChangeOfProtectionIsSaidAgain) {
  nevr_credential_stats::NoteSaved("host_mode_600");
  (void)Results();
  nevr_credential_stats::NoteSaved("host_mode_600");
  nevr_credential_stats::NoteSaved("dacl");
  const auto r = Results();
  ASSERT_EQ(r.size(), 2u);
  EXPECT_EQ(r[1]["observed"], "save ok protection=dacl saves=3 refused=0");
}

// L1: the review's scenario in the unit's own terms: results of the saves check use up its cap of 8, and a refusal
// still goes out, as its own result and not as "capped".
TEST_F(CredentialCacheCheck, ARefusalGetsThroughAfterTheSavesCheckHasBeenCapped) {
  for (int i = 0; i < 20; ++i) {
    nevr_credential_stats::NoteSaved(i % 2 == 0 ? "host_mode_600" : "dacl");  // each a change: each a result
    (void)Results();
  }
  const auto saves = Results();
  ASSERT_FALSE(saves.empty());
  EXPECT_EQ(saves.back()["observed"], "capped") << "the saves check really is capped in this scenario";
  nevr_credential_stats::NoteRefused("host mode is 644, not 600");
  const auto r = Results("credential_cache_refused");
  ASSERT_EQ(r.size(), 1u);
  EXPECT_EQ(r[0]["pass"], false);
  EXPECT_EQ(r[0]["observed"], "save refused why=host mode is 644, not 600 saves=20 refused=1");
}

TEST_F(CredentialCacheCheck, ReusedCredentialsPass) {
  nevr_credential_stats::NoteCacheReused();
  const auto r = Results();
  ASSERT_EQ(r.size(), 1u);
  EXPECT_EQ(r[0]["pass"], true);
  EXPECT_EQ(r[0]["observed"], "cache reused no_sign_in_needed");
}

// L2: "no cache" and "a cache that could not be used" are different things to the operator reading the result.
TEST_F(CredentialCacheCheck, ASignInWithNoCacheSaysSo) {
  nevr_credential_stats::NoteSignInRequired(SignInReason::kNoCache);
  const auto r = Results();
  ASSERT_EQ(r.size(), 1u);
  EXPECT_EQ(r[0]["pass"], true);
  EXPECT_EQ(r[0]["observed"], "cache not reused sign_in_required reason=no_cache");
}

TEST_F(CredentialCacheCheck, ASignInWithACachePresentButUnusableSaysSo) {
  nevr_credential_stats::NoteSignInRequired(SignInReason::kCacheUnusable);
  const auto r = Results();
  ASSERT_EQ(r.size(), 1u);
  EXPECT_EQ(r[0]["pass"], true);
  EXPECT_EQ(r[0]["observed"], "cache not reused sign_in_required reason=cache_present_unusable");
}

TEST_F(CredentialCacheCheck, EachChangeIsReportedOnce) {
  nevr_credential_stats::NoteCacheReused();
  EXPECT_EQ(Results().size(), 1u);
  EXPECT_EQ(Results().size(), 1u) << "nothing new, so nothing more";
}
