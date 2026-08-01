// test_nevr_config.cpp — unit tests for src/core/nevr_config (N133 S2).
//
// Exercises the parser in isolation (no game, no consumer): typed getters,
// the ordered plugins list with nested args, ${VAR:?} secret interpolation
// (set and unset->fail), ${VAR:-default}, unknown-top-level-key rejection,
// and malformed-YAML failure. Built under -DBUILD_TESTING=ON and run under
// Wine by `just test-auth-unit` (which `just verify` invokes).

#include <cstdlib>
#include <string>

#include <gtest/gtest.h>

#include "core/nevr_config.h"

namespace {

void SetEnv(const char* name, const char* value) { _putenv_s(name, value); }
// nevr_config treats an empty env value as unset, so "" is the unset case.
void UnsetEnv(const char* name) { _putenv_s(name, ""); }

// A representative config touching every schema section the getters read.
const char* kSampleYaml = R"YAML(
version: "1"
identity:
  discord_id: "1234567890"
  publisher_lock: ""
auth:
  http_key: "${NEVR_TEST_HTTP_KEY:?NEVR_TEST_HTTP_KEY must be set}"
  password: "${NEVR_TEST_PASSWORD:-defaultpass}"
services:
  serverdb: "wss://serverdb.example/nevr"
  matchmaking: "wss://mm.example/nevr"
network:
  external_ip: "203.0.113.7"
  upnp: true
  upnp_port: 6789
arena:
  round_time: 240.5
  mercy_score: 12
guilds:
  - "guild-a"
  - "guild-b"
  - "guild-c"
telemetry:
  uri: "wss://stream.example/ws"
plugins:
  - name: example
    enabled: true
    required: false
    args:
      greeting: "hello"
      nested:
        level: 3
        label: "deep"
  - name: token_auth
    file: token_auth.dll
    required: true
    target: server
x-custom:
  anything: "allowed"
)YAML";

TEST(NevrConfig, ParsesTypedScalars) {
  SetEnv("NEVR_TEST_HTTP_KEY", "secretvalue");
  UnsetEnv("NEVR_TEST_PASSWORD");
  const nevr::NevrConfig cfg = nevr::NevrConfig::LoadFromString(kSampleYaml);

  EXPECT_EQ(cfg.GetString("identity.discord_id").value_or(""), "1234567890");
  EXPECT_EQ(cfg.GetString("services.serverdb").value_or(""), "wss://serverdb.example/nevr");
  EXPECT_EQ(cfg.GetString("network.external_ip").value_or(""), "203.0.113.7");
  EXPECT_EQ(cfg.GetBool("network.upnp").value_or(false), true);
  EXPECT_EQ(cfg.GetInt("network.upnp_port").value_or(0), 6789);
  EXPECT_DOUBLE_EQ(cfg.GetFloat("arena.round_time").value_or(0.0), 240.5);
  EXPECT_EQ(cfg.GetInt("arena.mercy_score").value_or(0), 12);

  // Absent path / wrong type -> nullopt.
  EXPECT_FALSE(cfg.GetString("services.nonexistent").has_value());
  EXPECT_FALSE(cfg.GetInt("services.serverdb").has_value());
  EXPECT_FALSE(cfg.Empty());
}

TEST(NevrConfig, StringList) {
  SetEnv("NEVR_TEST_HTTP_KEY", "secretvalue");
  const nevr::NevrConfig cfg = nevr::NevrConfig::LoadFromString(kSampleYaml);
  const std::vector<std::string> guilds = cfg.GetStringList("guilds");
  ASSERT_EQ(guilds.size(), 3u);
  EXPECT_EQ(guilds[0], "guild-a");
  EXPECT_EQ(guilds[2], "guild-c");
}

TEST(NevrConfig, OrderedPluginsWithNestedArgs) {
  SetEnv("NEVR_TEST_HTTP_KEY", "secretvalue");
  const nevr::NevrConfig cfg = nevr::NevrConfig::LoadFromString(kSampleYaml);

  const std::vector<nevr::PluginSpec>& plugins = cfg.Plugins();
  ASSERT_EQ(plugins.size(), 2u);

  // Order preserved.
  EXPECT_EQ(plugins[0].name, "example");
  EXPECT_EQ(plugins[1].name, "token_auth");

  // file defaults to name + ".dll" when omitted.
  EXPECT_EQ(plugins[0].file, "example.dll");
  EXPECT_EQ(plugins[1].file, "token_auth.dll");

  EXPECT_TRUE(plugins[0].enabled);
  EXPECT_FALSE(plugins[0].required);
  EXPECT_TRUE(plugins[1].required);
  EXPECT_EQ(plugins[1].target, "server");

  // Nested args flatten to dotted keys.
  EXPECT_EQ(plugins[0].args.at("greeting"), "hello");
  EXPECT_EQ(plugins[0].args.at("nested.level"), "3");
  EXPECT_EQ(plugins[0].args.at("nested.label"), "deep");
}

TEST(NevrConfig, SecretInterpolationSet) {
  SetEnv("NEVR_TEST_HTTP_KEY", "a-real-secret");
  UnsetEnv("NEVR_TEST_PASSWORD");
  const nevr::NevrConfig cfg = nevr::NevrConfig::LoadFromString(kSampleYaml);
  EXPECT_EQ(cfg.GetString("auth.http_key").value_or(""), "a-real-secret");
  // ${VAR:-default} falls back when unset.
  EXPECT_EQ(cfg.GetString("auth.password").value_or(""), "defaultpass");
}

TEST(NevrConfig, RequiredSecretUnsetThrows) {
  UnsetEnv("NEVR_TEST_HTTP_KEY");  // referenced as ${...:?...}
  try {
    nevr::NevrConfig::LoadFromString(kSampleYaml);
    FAIL() << "expected NevrConfigError for an unset ${VAR:?} secret";
  } catch (const nevr::NevrConfigError& e) {
    // The :? message must propagate.
    EXPECT_NE(std::string(e.what()).find("NEVR_TEST_HTTP_KEY must be set"), std::string::npos);
  }
}

TEST(NevrConfig, BareRequiredVarUnsetThrows) {
  UnsetEnv("NEVR_TEST_BARE");
  EXPECT_THROW(nevr::NevrConfig::LoadFromString("services:\n  serverdb: \"${NEVR_TEST_BARE}\"\n"),
               nevr::NevrConfigError);
}

TEST(NevrConfig, DefaultInterpolationWhenUnset) {
  UnsetEnv("NEVR_TEST_OPT");
  const nevr::NevrConfig cfg =
      nevr::NevrConfig::LoadFromString("services:\n  serverdb: \"${NEVR_TEST_OPT:-fallback-host}\"\n");
  EXPECT_EQ(cfg.GetString("services.serverdb").value_or(""), "fallback-host");
}

TEST(NevrConfig, UnknownTopLevelKeyRejected) {
  EXPECT_THROW(nevr::NevrConfig::LoadFromString("bogus_section:\n  x: 1\n"), nevr::NevrConfigError);
}

TEST(NevrConfig, XPrefixTopLevelAllowed) {
  EXPECT_NO_THROW(nevr::NevrConfig::LoadFromString("x-overlay:\n  anything: true\n"));
}

TEST(NevrConfig, MalformedYamlThrows) {
  // Unbalanced flow bracket — a parser error, not a semantic one.
  EXPECT_THROW(nevr::NevrConfig::LoadFromString("services: [unterminated\n"),
               nevr::NevrConfigError);
}

TEST(NevrConfig, EmptyDocumentIsEmpty) {
  const nevr::NevrConfig cfg = nevr::NevrConfig::LoadFromString("");
  EXPECT_TRUE(cfg.Empty());
  EXPECT_TRUE(cfg.Plugins().empty());
}

TEST(NevrConfig, LoadFromFileOrFailClientReturnsEmpty) {
  // Client mode on a missing file: warns (not fatal) and returns an empty config.
  const nevr::NevrConfig cfg =
      nevr::NevrConfig::LoadFromFileOrFail("/definitely/not/a/real/config.yaml", /*is_server=*/false);
  EXPECT_TRUE(cfg.Empty());
}

}  // namespace
