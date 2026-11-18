// Tests for runtime/lifecycle/cdn_start_rules.h (PLAN-v7 5.4 G9): whether a pending self-update makes this start the trial, with the
// cause the start logs when pending.json exists and cannot be used, and which mounted sets a server hands to the read hook.
#include "runtime/lifecycle/cdn_start_rules.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace {

using nevr_cdn_start::DecideLastUpdateTrial;
using nevr_cdn_start::TrialDecision;
using nevr_update::ReadResult;

constexpr const char* kRunningSha = "2222222222222222222222222222222222222222222222222222222222222222";
constexpr const char* kOtherSha = "1111111111111111111111111111111111111111111111111111111111111111";
constexpr std::uint64_t kFirstTry = 1;
constexpr std::uint64_t kLastTry = nevr_update::kBootAttemptsToRollBack;

std::string Pending(const char* to_sha, std::uint64_t attempts) {
  return nlohmann::json{{"to", {{"version", "5.99.0"}, {"sha256", to_sha}}},
                        {"from", nullptr},
                        {"boot_attempts", attempts},
                        {"failed_reason", nullptr}}
      .dump();
}

struct Hasher {
  bool ok = true;
  int asked = 0;
  bool operator()(std::string* sha) {
    ++asked;
    if (!ok) return false;
    *sha = kRunningSha;
    return true;
  }
};

}  // namespace

TEST(CdnStartTrial, NoPendingFileIsNotATrialAndTheRuntimeIsNotHashed) {
  Hasher h;
  const TrialDecision d = DecideLastUpdateTrial(ReadResult::kAbsent, std::string(), std::ref(h));
  EXPECT_FALSE(d.trial);
  EXPECT_EQ(d.cause, nullptr);
  EXPECT_EQ(h.asked, 0);
}

TEST(CdnStartTrial, AFileThatExistsAndCannotBeReadOrParsedIsTheTrialWithItsCause) {
  Hasher h;
  TrialDecision d = DecideLastUpdateTrial(ReadResult::kUnreadable, std::string(), std::ref(h));
  EXPECT_TRUE(d.trial);
  EXPECT_STREQ(d.cause, "unreadable");
  d = DecideLastUpdateTrial(ReadResult::kTooBig, std::string(), std::ref(h));
  EXPECT_TRUE(d.trial);
  EXPECT_STREQ(d.cause, "too_big");
  d = DecideLastUpdateTrial(ReadResult::kOk, "{not json", std::ref(h));
  EXPECT_TRUE(d.trial);
  EXPECT_STREQ(d.cause, "unparsed");
  d = DecideLastUpdateTrial(ReadResult::kOk, "{\"to\":1}", std::ref(h));
  EXPECT_TRUE(d.trial) << "valid JSON that is not a pending record";
  EXPECT_STREQ(d.cause, "unparsed");
  EXPECT_EQ(h.asked, 0) << "the runtime is hashed only once the file parses";
}

TEST(CdnStartTrial, ARecordThatParsesIsTheTrialOnlyOnItsLastTryForThisRuntime) {
  Hasher h;
  TrialDecision d = DecideLastUpdateTrial(ReadResult::kOk, Pending(kRunningSha, kFirstTry), std::ref(h));
  EXPECT_FALSE(d.trial) << "the first try still fetches before boot";
  EXPECT_EQ(d.cause, nullptr);
  d = DecideLastUpdateTrial(ReadResult::kOk, Pending(kRunningSha, kLastTry), std::ref(h));
  EXPECT_TRUE(d.trial);
  EXPECT_EQ(d.cause, nullptr) << "a decision from the record carries no cause";
  d = DecideLastUpdateTrial(ReadResult::kOk, Pending(kOtherSha, kLastTry), std::ref(h));
  EXPECT_FALSE(d.trial) << "another release's record is not this runtime's trial";
  EXPECT_EQ(h.asked, 3);
}

TEST(CdnStartTrial, ARuntimeThatCannotBeHashedIsTheTrialWithItsCause) {
  Hasher h;
  h.ok = false;
  const TrialDecision d = DecideLastUpdateTrial(ReadResult::kOk, Pending(kRunningSha, kFirstTry), std::ref(h));
  EXPECT_TRUE(d.trial) << "even the first try: the safe side is no wait";
  EXPECT_STREQ(d.cause, "runtime_unhashed");
  EXPECT_EQ(h.asked, 1);
}

namespace {
struct FakeShadow {};
struct FakeSet {
  std::unique_ptr<FakeShadow> shadow;
};
}  // namespace

TEST(CdnStartPublish, OnlyASetThatHoldsAShadowIsPublished) {
  EXPECT_FALSE(nevr_cdn_start::HoldsShadow(static_cast<const FakeSet*>(nullptr)));
  FakeSet empty;
  EXPECT_FALSE(nevr_cdn_start::HoldsShadow(&empty)) << "nothing mounted: the set before it stays";
  FakeSet full;
  full.shadow = std::make_unique<FakeShadow>();
  EXPECT_TRUE(nevr_cdn_start::HoldsShadow(&full));
}
