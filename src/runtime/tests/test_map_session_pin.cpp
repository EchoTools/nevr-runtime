// The session pin's login entries as the PC login carries them (ADR 0048 decision 9): with nothing staged or mounted the login is
// byte-identical to today's, and with maps it carries `nevr_maps` with the entries the core builds. The pin's decisions and state
// are tested in src/core/tests/map_session_pin_test.cpp; this file reads the real login_profile.cpp.

#include <gtest/gtest.h>

#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/map_session_pin.h"
#include "nevr/symbol_hash.h"
#include "runtime/compat/login_profile.h"

namespace {

namespace sp = nevr_map_session_pin;

const std::string kSourceA = "0123456789abcdef" + std::string(48, 'a');
const std::string kSourceB = "fedcba9876543210" + std::string(48, 'b');

sp::MapPin Pin(const std::string& id, const std::string& version, const std::string& source) {
  sp::MapPin pin;
  pin.id = id;
  pin.version = version;
  pin.source_sha256 = source;
  pin.index_sha256 = std::string(64, '1');
  pin.platform = "windows-x86_64";
  pin.title = id;
  pin.level = EchoVR::CSymbol64Hash(id.c_str());
  return pin;
}

class MapSessionPinLogin : public ::testing::Test {
 protected:
  void SetUp() override { sp::ResetForTest(); }
  void TearDown() override { sp::ResetForTest(); }
};

TEST_F(MapSessionPinLogin, WithNothingStagedTheLoginIsUnchanged) {
  nevr_login_profile::LoginProfileInputs before;
  before.account_id = 42;
  before.display_name = "player";
  nevr_login_profile::LoginProfileInputs after = before;
  after.maps = sp::LoginEntries();
  EXPECT_EQ(nevr_login_profile::BuildLoginProfileJson(after), nevr_login_profile::BuildLoginProfileJson(before));

  const nlohmann::json profile = nlohmann::json::parse(nevr_login_profile::BuildLoginProfileJson(after));
  std::vector<std::string> keys;
  for (auto it = profile.begin(); it != profile.end(); ++it) keys.push_back(it.key());
  const std::vector<std::string> stock = {"access_token", "accountid",    "appid",          "buildversion",
                                          "bypassauth",   "desiredclientprofileversion",    "displayname",
                                          "hmdserialnumber", "lobbyversion", "nevr_identity", "nevr_plugins",
                                          "nevr_social",  "nonce",        "password",       "publisher_lock",
                                          "system_info"};
  EXPECT_EQ(keys, stock);
}

TEST_F(MapSessionPinLogin, TheLoginCarriesStagedAndMountedMaps) {
  sp::ChannelView view;
  view.staged = {Pin("arena2", "1.0", kSourceA), Pin("dock", "2.0", kSourceB)};
  sp::SetChannelView(view);
  sp::SetMountedPins({Pin("arena2", "1.0", kSourceA)});

  const nlohmann::json entries = sp::LoginEntries();
  ASSERT_EQ(entries.size(), 2u);
  EXPECT_EQ(entries[0]["id"], "arena2");
  EXPECT_EQ(entries[0]["mounted"], true);
  EXPECT_EQ(entries[1]["id"], "dock");
  EXPECT_EQ(entries[1]["mounted"], false);

  nevr_login_profile::LoginProfileInputs inputs;
  inputs.maps = entries;
  EXPECT_EQ(nlohmann::json::parse(nevr_login_profile::BuildLoginProfileJson(inputs))["nevr_maps"], entries);
}

}  // namespace
