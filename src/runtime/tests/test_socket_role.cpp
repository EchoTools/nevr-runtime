// #520: which of the game's sockets to the bridge is config, login or matchmaker. The decision is pure
// (compat/socket_role.h); the bridge's Open handler calling it, and the redirect putting the request
// targets on the bridge address, are driven for real in test_behavioral (RolesByPathTest) and
// test_service_config (TheRedirect...).

#include <gtest/gtest.h>

#include <string>

#include "runtime/compat/socket_role.h"

namespace {

using nevr_socket_role::Admission;
using nevr_socket_role::Advertisement;
using nevr_socket_role::Basis;
using nevr_socket_role::Listener;
using nevr_socket_role::Role;
using nevr_socket_role::RoleTable;

constexpr std::optional<Role> kNoTarget = std::nullopt;

void ExpectAdmission(const Admission& got, Role role, int connIdx, Basis basis) {
  EXPECT_EQ(got.role, role) << nevr_socket_role::RoleName(got.role);
  EXPECT_EQ(got.connIdx, connIdx);
  EXPECT_EQ(got.basis, basis) << nevr_socket_role::BasisName(got.basis);
}

TEST(SocketRoleTarget, TheTwoServicePathsNameTheirRole) {
  EXPECT_EQ(nevr_socket_role::RoleFromRequestTarget("/config"), Role::Config);
  EXPECT_EQ(nevr_socket_role::RoleFromRequestTarget("/login"), Role::Login);
}

TEST(SocketRoleTarget, AQueryOrFragmentDoesNotChangeThePath) {
  EXPECT_EQ(nevr_socket_role::RoleFromRequestTarget("/login?token=abc"), Role::Login);
  EXPECT_EQ(nevr_socket_role::RoleFromRequestTarget("/config#x"), Role::Config);
  EXPECT_EQ(nevr_socket_role::PathOf("/login?token=abc"), "/login");
}

TEST(SocketRoleTarget, NothingElseNamesARole) {
  for (const char* target : {"", "/", "/rad/rad15_live", "/Login", "/login/", "/loginx", "/config/x", "login", "?/login"}) {
    EXPECT_FALSE(nevr_socket_role::RoleFromRequestTarget(target).has_value()) << target;
  }
}

TEST(SocketRoleKeys, TheConfigAndLoginServiceKeysNameTheirRole) {
  for (const char* key : {"config_host", "configservice_host"}) {
    EXPECT_EQ(nevr_socket_role::RoleForServiceKey(key), Role::Config) << key;
  }
  for (const char* key : {"login_host", "loginservice_host"}) {
    EXPECT_EQ(nevr_socket_role::RoleForServiceKey(key), Role::Login) << key;
  }
  for (const char* key : {"matchingservice_host", "serverdb_host", "apiservice_host", ""}) {
    EXPECT_FALSE(nevr_socket_role::RoleForServiceKey(key).has_value()) << key;
  }
  EXPECT_FALSE(nevr_socket_role::RoleForServiceKey(nullptr).has_value());
}

TEST(SocketRoleTag, TheBridgeAddressGetsTheServicesTarget) {
  Advertisement advertised;
  EXPECT_EQ(nevr_socket_role::TagBridgeUrl("ws://127.0.0.1:53748", 53748, "loginservice_host", advertised),
            "ws://127.0.0.1:53748/login");
  EXPECT_EQ(nevr_socket_role::TagBridgeUrl("ws://127.0.0.1:53748", 53748, "configservice_host", advertised),
            "ws://127.0.0.1:53748/config");
  EXPECT_TRUE(advertised.Has(Role::Login));
  EXPECT_TRUE(advertised.Has(Role::Config));
}

TEST(SocketRoleTag, AnythingElseIsLeftAloneAndAdvertisesNothing) {
  Advertisement advertised;
  // Another service's key, another host, another port, no key, no port: not the bridge address of a role.
  EXPECT_EQ(nevr_socket_role::TagBridgeUrl("ws://127.0.0.1:53748", 53748, "matchingservice_host", advertised),
            "ws://127.0.0.1:53748");
  EXPECT_EQ(nevr_socket_role::TagBridgeUrl("ws://g.echovrce.com:80/spr", 53748, "loginservice_host", advertised),
            "ws://g.echovrce.com:80/spr");
  EXPECT_EQ(nevr_socket_role::TagBridgeUrl("ws://127.0.0.1:1", 53748, "loginservice_host", advertised),
            "ws://127.0.0.1:1");
  EXPECT_EQ(nevr_socket_role::TagBridgeUrl("ws://127.0.0.1:53748", 53748, nullptr, advertised),
            "ws://127.0.0.1:53748");
  EXPECT_EQ(nevr_socket_role::TagBridgeUrl("ws://127.0.0.1:0", 0, "loginservice_host", advertised),
            "ws://127.0.0.1:0");
  EXPECT_FALSE(advertised.Has(Role::Login));
  EXPECT_FALSE(advertised.Has(Role::Config));
}

TEST(SocketRoleTag, AnAddressTheBridgeIsGivenBackReadsAsItsRole) {
  // The round trip the two halves rely on: what the redirect tags, the bridge reads.
  Advertisement advertised;
  const std::string login = nevr_socket_role::TagBridgeUrl("ws://127.0.0.1:50000", 50000, "login_host", advertised);
  const std::string config = nevr_socket_role::TagBridgeUrl("ws://127.0.0.1:50000", 50000, "config_host", advertised);
  EXPECT_EQ(nevr_socket_role::RoleFromRequestTarget(login.substr(login.find('/', 6))), Role::Login);
  EXPECT_EQ(nevr_socket_role::RoleFromRequestTarget(config.substr(config.find('/', 6))), Role::Config);
}

// The case the issue is about: the login socket connects first.
TEST(SocketRoleTable, ALoginSocketThatConnectsFirstIsTheLogin) {
  Advertisement advertised;
  RoleTable table(&advertised);
  ExpectAdmission(table.Admit(Listener::Service, Role::Login), Role::Login, 1, Basis::Path);
  ExpectAdmission(table.Admit(Listener::Service, Role::Config), Role::Config, 0, Basis::Path);
}

TEST(SocketRoleTable, ConfigThenLoginIsTheSameAssignment) {
  Advertisement advertised;
  RoleTable table(&advertised);
  ExpectAdmission(table.Admit(Listener::Service, Role::Config), Role::Config, 0, Basis::Path);
  ExpectAdmission(table.Admit(Listener::Service, Role::Login), Role::Login, 1, Basis::Path);
}

// N91: two sockets claiming login must not both inject. Only the socket that takes the login slot is a
// login; the next shares the session.
TEST(SocketRoleTable, ASecondLoginClaimSharesTheSessionInsteadOfLoggingInAgain) {
  Advertisement advertised;
  RoleTable table(&advertised);
  ExpectAdmission(table.Admit(Listener::Service, Role::Login), Role::Login, 1, Basis::Path);
  ExpectAdmission(table.Admit(Listener::Service, Role::Login), Role::Matchmaker, 2, Basis::LoginClaimed);
  ExpectAdmission(table.Admit(Listener::Service, Role::Login), Role::Matchmaker, 3, Basis::LoginClaimed);
  EXPECT_TRUE(table.LoginClaimed());
}

// A /config after login is a config socket: no shared index, not a matchmaker.
TEST(SocketRoleTable, ALateConfigAfterLoginIsStillConfig) {
  Advertisement advertised;
  RoleTable table(&advertised);
  table.Admit(Listener::Service, Role::Config);
  table.Admit(Listener::Service, Role::Login);
  ExpectAdmission(table.Admit(Listener::Service, Role::Config), Role::Config, 0, Basis::Path);
  // And the next matchmaker still gets the first shared index.
  ExpectAdmission(table.Admit(Listener::Matchmaker, kNoTarget), Role::Matchmaker, 2, Basis::Listener);
}

// #70: when the login session ends the game's next /login is a login again, and the shared numbering restarts.
TEST(SocketRoleTable, AfterTheLoginSessionEndsTheNextLoginIsALoginAgain) {
  Advertisement advertised;
  RoleTable table(&advertised);
  table.Admit(Listener::Service, Role::Config);
  table.Admit(Listener::Service, Role::Login);
  table.Admit(Listener::Matchmaker, kNoTarget);
  EXPECT_EQ(table.NextSharedConnIdx(), 3);
  table.LoginSessionEnded();
  EXPECT_FALSE(table.LoginClaimed());
  EXPECT_EQ(table.NextSharedConnIdx(), 2);
  ExpectAdmission(table.Admit(Listener::Service, Role::Login), Role::Login, 1, Basis::Path);
}

TEST(SocketRoleTable, TheMatchmakerListenerMakesAMatchmakerWhateverTheTarget) {
  Advertisement advertised;
  RoleTable table(&advertised);
  ExpectAdmission(table.Admit(Listener::Matchmaker, Role::Login), Role::Matchmaker, 2, Basis::Listener);
  ExpectAdmission(table.Admit(Listener::Matchmaker, kNoTarget), Role::Matchmaker, 3, Basis::Listener);
  EXPECT_FALSE(table.LoginClaimed());
}

// Nothing tagged and nothing advertised: the connect order, config then login, as before.
TEST(SocketRoleTable, SocketsWithNoTargetKeepTheConnectOrderWhenNothingIsTagged) {
  Advertisement advertised;
  RoleTable table(&advertised);
  ExpectAdmission(table.Admit(Listener::Service, kNoTarget), Role::Config, 0, Basis::Order);
  ExpectAdmission(table.Admit(Listener::Service, kNoTarget), Role::Login, 1, Basis::Order);
  ExpectAdmission(table.Admit(Listener::Service, kNoTarget), Role::Matchmaker, 2, Basis::Order);
}

// A pathless socket must not take the config or login slot while a role-tagged one is expected.
TEST(SocketRoleTable, APathlessSocketDoesNotTakeASlotATaggedSocketIsExpectedFor) {
  Advertisement advertised;
  advertised.Note(Role::Config);
  advertised.Note(Role::Login);
  RoleTable table(&advertised);
  ExpectAdmission(table.Admit(Listener::Service, kNoTarget), Role::Matchmaker, 2, Basis::SlotReserved);
  // The tagged sockets still get their slots afterwards.
  ExpectAdmission(table.Admit(Listener::Service, Role::Login), Role::Login, 1, Basis::Path);
  ExpectAdmission(table.Admit(Listener::Service, Role::Config), Role::Config, 0, Basis::Path);
}

// Only the advertised roles are reserved: a pathless socket may still take the other slot by order.
TEST(SocketRoleTable, APathlessSocketTakesTheSlotNothingIsExpectedFor) {
  Advertisement advertised;
  advertised.Note(Role::Config);  // the config service is tagged, the login service is not
  RoleTable table(&advertised);
  ExpectAdmission(table.Admit(Listener::Service, kNoTarget), Role::Login, 1, Basis::Order);
  // The login slot is now taken; the config slot is still reserved for a /config socket.
  ExpectAdmission(table.Admit(Listener::Service, kNoTarget), Role::Matchmaker, 2, Basis::SlotReserved);
  ExpectAdmission(table.Admit(Listener::Service, Role::Config), Role::Config, 0, Basis::Path);
}

TEST(SocketRoleTable, ResetForgetsEverySlot) {
  Advertisement advertised;
  RoleTable table(&advertised);
  table.Admit(Listener::Service, Role::Config);
  table.Admit(Listener::Service, Role::Login);
  table.Admit(Listener::Matchmaker, kNoTarget);
  table.Reset();
  EXPECT_FALSE(table.LoginClaimed());
  EXPECT_EQ(table.NextSharedConnIdx(), 2);
  ExpectAdmission(table.Admit(Listener::Service, kNoTarget), Role::Config, 0, Basis::Order);
}

TEST(SocketRoleFirstFrame, TheFramesThatNameARole) {
  using namespace nevr_evr_codec;
  EXPECT_EQ(nevr_socket_role::RoleOfFirstSymbol(kSymConfigRequest), Role::Config);
  EXPECT_EQ(nevr_socket_role::RoleOfFirstSymbol(kSymLoginRequest), Role::Login);
  EXPECT_EQ(nevr_socket_role::RoleOfFirstSymbol(kSymFindSessionRequest), Role::Matchmaker);
  EXPECT_EQ(nevr_socket_role::RoleOfFirstSymbol(kSymLobbyPingResponse), Role::Matchmaker);
  // Any other symbol says nothing: the login connection can send anything.
  EXPECT_FALSE(nevr_socket_role::RoleOfFirstSymbol(0x1234).has_value());
  EXPECT_FALSE(nevr_socket_role::RoleOfFirstSymbol(0).has_value());
}

}  // namespace
