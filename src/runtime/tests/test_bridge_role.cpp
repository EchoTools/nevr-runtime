// #513: a game socket's role on the bridge comes from the path its service's redirect gave it, not from the
// order the bridge accepted the sockets in.
#include "runtime/compat/bridge_role.h"

#include <gtest/gtest.h>

#include <atomic>

namespace {

using nevr_bridge_role::ConnectionIndexFor;
using nevr_bridge_role::PathForServiceKey;
using nevr_bridge_role::Role;
using nevr_bridge_role::RoleFromRequestPath;

TEST(BridgeRole, TheConfigAndLoginKeysNameTheirPath) {
  EXPECT_STREQ(PathForServiceKey("config_host"), "/config");
  EXPECT_STREQ(PathForServiceKey("configservice_host"), "/config");
  EXPECT_STREQ(PathForServiceKey("login_host"), "/login");
  EXPECT_STREQ(PathForServiceKey("loginservice_host"), "/login");
  for (const char* other :
       {"transaction_host", "transactionservice_host", "matchingservice_host", "serverdb_host", "login", ""}) {
    EXPECT_STREQ(PathForServiceKey(other), "") << other;
  }
  EXPECT_STREQ(PathForServiceKey(nullptr), "");
}

TEST(BridgeRole, TheRequestPathNamesTheRole) {
  EXPECT_EQ(RoleFromRequestPath("/config"), Role::Config);
  EXPECT_EQ(RoleFromRequestPath("/config/"), Role::Config);
  EXPECT_EQ(RoleFromRequestPath("/config?format=evr"), Role::Config);
  EXPECT_EQ(RoleFromRequestPath("/login"), Role::Login);
  EXPECT_EQ(RoleFromRequestPath("/login?format=evr"), Role::Login);
  for (const char* other : {"/", "", "/configx", "/loginservice", "/rad/rad15_live", "config", "/match"}) {
    EXPECT_EQ(RoleFromRequestPath(other), Role::Unknown) << other;
  }
}

TEST(BridgeRole, ALoginSocketAcceptedFirstIsStillTheLogin) {
  // The stalled start of #513: both sockets in the same millisecond, the login socket accepted first.
  std::atomic<int> counter{0};
  EXPECT_EQ(ConnectionIndexFor(Role::Login, false, counter), 1);
  EXPECT_EQ(ConnectionIndexFor(Role::Config, false, counter), 0);
  EXPECT_EQ(ConnectionIndexFor(Role::Unknown, true, counter), 2);  // the next socket comes after the login
}

TEST(BridgeRole, ConfigThenLoginKeepTheirIndices) {
  std::atomic<int> counter{0};
  EXPECT_EQ(ConnectionIndexFor(Role::Config, false, counter), 0);
  EXPECT_EQ(ConnectionIndexFor(Role::Login, false, counter), 1);
  EXPECT_EQ(ConnectionIndexFor(Role::Unknown, true, counter), 2);
  EXPECT_EQ(ConnectionIndexFor(Role::Unknown, true, counter), 3);
}

TEST(BridgeRole, SocketsWithNoRoleAreNumberedByConnectOrderAsBefore) {
  std::atomic<int> counter{0};
  for (int expected = 0; expected < 4; ++expected) {
    EXPECT_EQ(ConnectionIndexFor(Role::Unknown, expected >= 2, counter), expected);
  }
}

TEST(BridgeRole, ALoginSocketWhileTheLoginSessionIsOpenSharesItAsBefore) {
  std::atomic<int> counter{3};
  EXPECT_EQ(ConnectionIndexFor(Role::Login, true, counter), 3);
  EXPECT_EQ(counter.load(), 4);
}

TEST(BridgeRole, ALoginAfterTheSessionEndedIsTheLoginAgain) {
  // When the login session ends the bridge renumbers from 1 (ForgetLoginSessionLocked, #70).
  std::atomic<int> counter{1};
  EXPECT_EQ(ConnectionIndexFor(Role::Login, false, counter), 1);
  EXPECT_EQ(ConnectionIndexFor(Role::Unknown, true, counter), 2);
}

TEST(BridgeRole, ALaterConfigSocketLeavesTheCounter) {
  std::atomic<int> counter{5};
  EXPECT_EQ(ConnectionIndexFor(Role::Config, true, counter), 0);
  EXPECT_EQ(counter.load(), 5);
}

}  // namespace
