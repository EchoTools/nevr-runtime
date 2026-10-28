// #1254: what a refused login does. The decisions are pure (compat/login_refusal.h); the bridge calling
// them, with the real sockets, is driven in test_behavioral (LoginRefusalTest) and pinned by
// tools/tests/test_login_refusal_wiring.py.

#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "runtime/compat/login_refusal.h"

namespace {

using nevr_login_refusal::ClientRefusalLatch;
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

}  // namespace
