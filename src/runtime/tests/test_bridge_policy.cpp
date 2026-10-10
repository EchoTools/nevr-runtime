// #16: a dedicated server without a login bridge must refuse to start unless the operator opted in.

#include <gtest/gtest.h>

#include "core/nevr_config.h"
#include "runtime/lifecycle/bridge_policy.h"
#include "runtime/lifecycle/service_map.h"

namespace {

using BridgePolicy::Decide;
using BridgePolicy::Outcome;

TEST(BridgePolicy, ConfiguredSocketUriAlwaysStartsTheBridge) {
  EXPECT_EQ(Decide(true, true, false), Outcome::Start);
  EXPECT_EQ(Decide(true, false, false), Outcome::Start);
  EXPECT_EQ(Decide(true, true, true), Outcome::Start);
}

TEST(BridgePolicy, ServerWithoutBridgeIsRefusedUnlessOptedIn) {
  EXPECT_EQ(Decide(false, true, false), Outcome::RefuseServer);
  EXPECT_EQ(Decide(false, true, true), Outcome::SkipOfflineServer);
}

TEST(BridgePolicy, ClientWithoutBridgeKeepsRunning) {
  EXPECT_EQ(Decide(false, false, false), Outcome::SkipClient);
  EXPECT_EQ(Decide(false, false, true), Outcome::SkipClient);
}

TEST(BridgePolicy, OnlyExplicitTruthyValuesOptIn) {
  EXPECT_TRUE(BridgePolicy::IsTruthy("true"));
  EXPECT_TRUE(BridgePolicy::IsTruthy("1"));
  EXPECT_FALSE(BridgePolicy::IsTruthy("false"));
  EXPECT_FALSE(BridgePolicy::IsTruthy(""));
  EXPECT_FALSE(BridgePolicy::IsTruthy(nullptr));
  EXPECT_FALSE(BridgePolicy::IsTruthy("maybe"));
}

// #245: the spelling rule is config.yaml's, which is case-insensitive.
TEST(BridgePolicy, TruthySpellingsAreCaseInsensitive) {
  for (const char* v : {"Yes", "YES", "ON", "On", "tRuE"}) EXPECT_TRUE(BridgePolicy::IsTruthy(v)) << v;
  for (const char* v : {"No", "OFF", "False"}) EXPECT_FALSE(BridgePolicy::IsTruthy(v)) << v;
}

TEST(BridgePolicy, OnlyAnUnparseableValueIsReportedAsUnrecognized) {
  EXPECT_TRUE(BridgePolicy::IsUnrecognized("maybe"));
  EXPECT_TRUE(BridgePolicy::IsUnrecognized("ture"));
  EXPECT_FALSE(BridgePolicy::IsUnrecognized("Yes"));
  EXPECT_FALSE(BridgePolicy::IsUnrecognized("off"));
  EXPECT_FALSE(BridgePolicy::IsUnrecognized(""));
  EXPECT_FALSE(BridgePolicy::IsUnrecognized(nullptr));
}

// The key is read through the flat-key table, so a config.yaml `services.allow_offline_server: true`
// reaches the decision.
TEST(BridgePolicy, ConfigKeyIsReadFromServicesAllowOfflineServer) {
  EXPECT_EQ(nevr_cfg::FlatKeyToYamlPath("nevr_allow_offline_server"), "services.allow_offline_server");
  const auto cfg = nevr::NevrConfig::LoadFromString("services:\n  allow_offline_server: true\n");
  const std::string on = nevr_cfg::LookupFlat(cfg, "nevr_allow_offline_server").value_or("");
  EXPECT_TRUE(BridgePolicy::IsTruthy(on.c_str()));
  const auto none = nevr::NevrConfig::LoadFromString("services:\n  loginservice_host: \"ws://x\"\n");
  const std::string off = nevr_cfg::LookupFlat(none, "nevr_allow_offline_server").value_or("");
  EXPECT_FALSE(BridgePolicy::IsTruthy(off.c_str()));
}

}  // namespace
