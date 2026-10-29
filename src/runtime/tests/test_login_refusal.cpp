// #1254: what a refused login does. The decisions are pure (compat/login_refusal.h); the bridge calling
// them, with the real sockets, is driven in test_behavioral (LoginRefusalTest) and pinned by
// tools/tests/test_login_refusal_wiring.py.

#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "runtime/compat/login_refusal.h"

namespace {

using nevr_login_refusal::AnswersLoginLocally;
using nevr_login_refusal::ClientRefusalLatch;
using nevr_login_refusal::LocalAnswerCounter;
using nevr_login_refusal::DescribeConfigSource;
using nevr_login_refusal::MissingServerCredentialKeys;
using nevr_login_refusal::NoCredentialCause;
using nevr_login_refusal::RefusedLoginCause;

TEST(LoginRefusalMissing, ABothSetServerMissesNothing) {
  EXPECT_TRUE(MissingServerCredentialKeys("1234567890", "hunter2").empty());
}

TEST(LoginRefusalMissing, ANoDiscordIdNamesTheDiscordIdKey) {
  const std::vector<std::string> missing = MissingServerCredentialKeys("", "hunter2");
  ASSERT_EQ(missing.size(), 1u);
  EXPECT_EQ(missing[0], "identity.discord_id");
}

TEST(LoginRefusalMissing, ANoPasswordNamesThePasswordKey) {
  const std::vector<std::string> missing = MissingServerCredentialKeys("1234567890", "");
  ASSERT_EQ(missing.size(), 1u);
  EXPECT_EQ(missing[0], "auth.password");
}

TEST(LoginRefusalMissing, NeitherSetNamesBothInOrder) {
  const std::vector<std::string> missing = MissingServerCredentialKeys("", "");
  ASSERT_EQ(missing.size(), 2u);
  EXPECT_EQ(missing[0], "identity.discord_id");
  EXPECT_EQ(missing[1], "auth.password");
}

TEST(LoginRefusalCause, TheNoCredentialCauseNamesEveryMissingKeyAndTheConfig) {
  const std::string cause = NoCredentialCause({"identity.discord_id", "auth.password"}, "C:\\nevr\\config.yaml");
  EXPECT_NE(cause.find("identity.discord_id"), std::string::npos) << cause;
  EXPECT_NE(cause.find("auth.password"), std::string::npos) << cause;
  EXPECT_NE(cause.find("C:\\nevr\\config.yaml"), std::string::npos) << cause;
}

TEST(LoginRefusalCause, NoConfigFoundIsSaidAsSuch) {
  const std::string cause = NoCredentialCause({"auth.password"}, "");
  EXPECT_NE(cause.find("config: none found"), std::string::npos) << cause;
}

TEST(LoginRefusalCause, TheSearchedDirectoriesTheCallerNamesAreKept) {
  const std::string cause = NoCredentialCause({"auth.password"}, "none found, searched C:\\a, C:\\b");
  EXPECT_NE(cause.find("config: none found, searched C:\\a, C:\\b;"), std::string::npos) << cause;
}

TEST(LoginRefusalCause, TheStopLineIsOneLineThatSaysWhatToSet) {
  const std::string cause = NoCredentialCause({"identity.discord_id", "auth.password"}, "C:\\nevr\\config.yaml");
  EXPECT_EQ(cause.rfind("server login refused: ", 0), 0u) << cause;
  EXPECT_EQ(cause.find('\n'), std::string::npos) << cause;
  EXPECT_NE(cause.find("set identity.discord_id, auth.password in config.yaml"), std::string::npos) << cause;
}

TEST(LoginRefusalCause, TheRefusedCauseCarriesTheStatusAndTheConfig) {
  const std::string cause = RefusedLoginCause(401, "C:\\nevr\\config.yaml");
  EXPECT_NE(cause.find("401"), std::string::npos) << cause;
  EXPECT_NE(cause.find("C:\\nevr\\config.yaml"), std::string::npos) << cause;
  EXPECT_NE(cause.find("identity.discord_id"), std::string::npos) << cause;
  EXPECT_NE(cause.find("auth.password"), std::string::npos) << cause;
}

TEST(LoginRefusalCause, TheRefusedCauseIsOneLineAndNamesNoConfigWhenNoneWasFound) {
  const std::string cause = RefusedLoginCause(403, "");
  EXPECT_EQ(cause.find('\n'), std::string::npos) << cause;
  EXPECT_NE(cause.find("config: none found"), std::string::npos) << cause;
}

TEST(LoginRefusalConfigSource, ALoadedConfigIsNamedByItsPath) {
  EXPECT_EQ(DescribeConfigSource("C:\\nevr\\_local\\config.yaml", {"C:\\x", "C:\\y"}), "C:\\nevr\\_local\\config.yaml");
}

TEST(LoginRefusalConfigSource, NoConfigNamesWhereItLooked) {
  EXPECT_EQ(DescribeConfigSource("", {"C:\\x\\config.yaml", "C:\\y\\config.yaml"}),
            "none found, searched C:\\x\\config.yaml, C:\\y\\config.yaml");
}

TEST(LoginRefusalConfigSource, NoConfigAndNoSearchListIsJustNoneFound) {
  EXPECT_EQ(DescribeConfigSource("", {}), "none found");
}

TEST(LoginRefusalStatus, OnlyTheAuthStatusesAreARefusal) {
  EXPECT_TRUE(nevr_login_refusal::IsLoginRefusalStatus(401));
  EXPECT_TRUE(nevr_login_refusal::IsLoginRefusalStatus(403));
  EXPECT_FALSE(nevr_login_refusal::IsLoginRefusalStatus(0));
  EXPECT_FALSE(nevr_login_refusal::IsLoginRefusalStatus(404));
  EXPECT_FALSE(nevr_login_refusal::IsLoginRefusalStatus(500));
  EXPECT_FALSE(nevr_login_refusal::IsLoginRefusalStatus(502));
}

TEST(LoginRefusalLatch, TheFirstRefusalOfABearerIsSaidOnce) {
  ClientRefusalLatch latch;
  EXPECT_TRUE(latch.ShouldSay("token-A"));
  EXPECT_FALSE(latch.ShouldSay("token-A"));
  EXPECT_FALSE(latch.ShouldSay("token-A"));
}

TEST(LoginRefusalLatch, ANewBearerIsSaidAgain) {
  ClientRefusalLatch latch;
  EXPECT_TRUE(latch.ShouldSay("token-A"));
  EXPECT_TRUE(latch.ShouldSay("token-B"));
  EXPECT_FALSE(latch.ShouldSay("token-B"));
}

TEST(LoginRefusalLatch, AnAlreadyRefusedBearerIsRefusedWithoutANetworkTrip) {
  ClientRefusalLatch latch;
  EXPECT_FALSE(latch.IsRefused("token-A"));
  (void)latch.ShouldSay("token-A");
  EXPECT_TRUE(latch.IsRefused("token-A"));
  EXPECT_FALSE(latch.IsRefused("token-B"));
}

TEST(LoginRefusalLatch, AnUnauthenticatedClientIsLatchedLikeAnyOther) {
  ClientRefusalLatch latch;
  EXPECT_FALSE(latch.IsRefused(""));
  EXPECT_TRUE(latch.ShouldSay(""));
  EXPECT_FALSE(latch.ShouldSay(""));
  EXPECT_TRUE(latch.IsRefused(""));
  EXPECT_TRUE(latch.ShouldSay("token-A"));
}

// #1446: a client whose sign-in ended without a token answers the game's login itself. Only that case: a server,
// a config or matchmaker socket, a sign-in that still carries a token or URL credentials, or a sign-in still running
// keeps the path that asks the service.
TEST(LoginRefusalLocalAnswer, OnlyAClientLoginAfterTheSignInEndedWithoutATokenIsAnsweredHere) {
  EXPECT_TRUE(AnswersLoginLocally(/*isServer=*/false, /*isLoginSocket=*/true, /*bearerPresent=*/false,
                                  /*urlCredentials=*/false, /*signInEnded=*/true));
  EXPECT_FALSE(AnswersLoginLocally(true, true, false, false, true)) << "a server has its own rule";
  EXPECT_FALSE(AnswersLoginLocally(false, false, false, false, true)) << "a config or matchmaker socket";
  EXPECT_FALSE(AnswersLoginLocally(false, true, true, false, true)) << "a token is presented";
  EXPECT_FALSE(AnswersLoginLocally(false, true, false, true, true)) << "URL credentials are presented";
  EXPECT_FALSE(AnswersLoginLocally(false, true, false, false, false)) << "the sign-in has not ended";
}

// #1446: the first answer is written at once; the answers after it are counted and written once per window.
TEST(LoginRefusalLocalAnswer, TheFirstAnswerIsWrittenAndLaterOnesAreCountedOncePerWindow) {
  LocalAnswerCounter counter;
  const LocalAnswerCounter::Verdict first = counter.Record(0);
  EXPECT_TRUE(first.write);
  EXPECT_TRUE(first.first);
  EXPECT_EQ(first.count, 1u);
  EXPECT_FALSE(counter.Record(1000).write);
  EXPECT_FALSE(counter.Record(59999).write) << "inside the window nothing is written";
  const LocalAnswerCounter::Verdict summary = counter.Record(60000);
  EXPECT_TRUE(summary.write);
  EXPECT_FALSE(summary.first);
  EXPECT_EQ(summary.count, 3u) << "the answers since the last line, this one included";
  EXPECT_FALSE(counter.Record(60001).write);
  const LocalAnswerCounter::Verdict next = counter.Record(120000);
  EXPECT_TRUE(next.write);
  EXPECT_EQ(next.count, 2u);
}

}  // namespace
