// test_nevr_config.cpp — unit tests for src/core/nevr_config (N133 S2).
//
// Exercises the parser in isolation (no game, no consumer): typed getters,
// the ordered plugins list with nested args, ${VAR:?} secret interpolation
// (set and unset->fail), ${VAR:-default}, unknown-top-level-key rejection,
// and malformed-YAML failure. Built under -DBUILD_TESTING=ON and run under
// Wine by `just test-auth-unit` (which `just verify` invokes).

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstdio>
#include <fstream>
#include <string>
#include <utility>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#  define NOMINMAX
#endif
#include <windows.h>

#include <gtest/gtest.h>

#include "core/nevr_config.h"
#include "core/logging.h"

namespace {

void SetEnv(const char* name, const char* value) { _putenv_s(name, value); }
// nevr_config treats an empty env value as unset, so "" is the unset case.
void UnsetEnv(const char* name) { _putenv_s(name, ""); }

// Where the fixture files go, as a Windows path ending in a backslash. $NEVR_CONFIG_TEST_DIR names it
// (`just test-auth-unit` sets it to a directory it removes afterwards); a Linux path is reached through
// Wine's Z: drive. Without it the process's own temporary directory is used. Never the game directory.
std::string ScratchConfigDirectory() {
  const char* configured = std::getenv("NEVR_CONFIG_TEST_DIR");
  if (configured == nullptr || configured[0] == '\0') {
    char temp[MAX_PATH + 1] = {};
    const DWORD length = GetTempPathA(static_cast<DWORD>(sizeof(temp)), temp);
    return std::string(temp, length) + "nevr-config-tests\\";
  }
  std::string directory(configured);
  if (directory[0] == '/') {
    directory = "Z:" + directory;
    for (char& c : directory) {
      if (c == '/') c = '\\';
    }
  }
  if (directory.back() != '\\') directory += '\\';
  return directory;
}

// Creates each missing level ("Z:\var", "Z:\var\tmp", ...): CreateDirectoryA makes one level at a time,
// and a fresh machine (the CI container, run 37151801289) has only some of them.
bool EnsureScratchConfigDirectory() {
  const std::string full = ScratchConfigDirectory();
  for (size_t sep = full.find('\\', 3); sep != std::string::npos; sep = full.find('\\', sep + 1)) {
    const std::string level = full.substr(0, sep);
    if (CreateDirectoryA(level.c_str(), nullptr) == FALSE && GetLastError() != ERROR_ALREADY_EXISTS) return false;
  }
  return true;
}

std::string ScratchConfigPath(const char* name) {
  return ScratchConfigDirectory() + name + "-" + std::to_string(GetCurrentProcessId()) +
         ".yaml";
}

class ScopedScratchFile {
 public:
  explicit ScopedScratchFile(std::string path) : path_(std::move(path)) {}
  ~ScopedScratchFile() { std::remove(path_.c_str()); }

  const std::string& path() const { return path_; }

 private:
  std::string path_;
};

std::string g_configFatalMessage;

void CaptureConfigFatal(const CHAR* message, const CHAR*) { g_configFatalMessage = message; }

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

// S4a — auth.password is the ws_bridge login secret. Pin its required-by-ref
// contract on the exact key that ships: ${NEVR_PASSWORD:?} resolves when the env
// is set, and fails loud (throws NevrConfigError -> ServerFatal at the singleton)
// when unset. An empty env is treated as unset (never send an empty secret, N115).
TEST(NevrConfig, AuthPasswordRequiredRefResolvesWhenSet) {
  SetEnv("NEVR_S4A_PASSWORD", "a-real-server-password");
  const nevr::NevrConfig cfg = nevr::NevrConfig::LoadFromString(
      "auth:\n  password: \"${NEVR_S4A_PASSWORD:?NEVR_S4A_PASSWORD must be set for server login}\"\n");
  EXPECT_EQ(cfg.GetString("auth.password").value_or(""), "a-real-server-password");
}

TEST(NevrConfig, AuthPasswordRequiredRefUnsetFailsLoud) {
  UnsetEnv("NEVR_S4A_PASSWORD");  // empty == unset
  try {
    nevr::NevrConfig::LoadFromString(
        "auth:\n  password: \"${NEVR_S4A_PASSWORD:?NEVR_S4A_PASSWORD must be set for server login}\"\n");
    FAIL() << "expected NevrConfigError for an unset ${NEVR_PASSWORD:?} secret";
  } catch (const nevr::NevrConfigError& e) {
    EXPECT_NE(std::string(e.what()).find("NEVR_S4A_PASSWORD must be set for server login"),
              std::string::npos);
  }
}

// S5 — auth.server_key is a token_auth secret. It reads through the module config
// accessor (ctx->config_get -> NevrCfgGetFlat), which shares this singleton's
// mode-aware fail-loud: a ${VAR:?} secret resolves when set, and throws
// NevrConfigError (-> ServerFatal, server=fatal/client=warn) when unset. Locks the
// contract on the exact key S5 adds. (token_auth reads server_key only on a CLIENT,
// where ServerFatal warns rather than exits — see crash_recovery.cpp ServerFatal.)
TEST(NevrConfig, AuthServerKeyRequiredRefResolvesWhenSet) {
  SetEnv("NEVR_S5_SERVER_KEY", "a-real-server-key");
  const nevr::NevrConfig cfg = nevr::NevrConfig::LoadFromString(
      "auth:\n  server_key: \"${NEVR_S5_SERVER_KEY:?NEVR_S5_SERVER_KEY must be set}\"\n");
  EXPECT_EQ(cfg.GetString("auth.server_key").value_or(""), "a-real-server-key");
}

TEST(NevrConfig, AuthServerKeyRequiredRefUnsetFailsLoud) {
  UnsetEnv("NEVR_S5_SERVER_KEY");  // empty == unset
  try {
    nevr::NevrConfig::LoadFromString(
        "auth:\n  server_key: \"${NEVR_S5_SERVER_KEY:?NEVR_S5_SERVER_KEY must be set}\"\n");
    FAIL() << "expected NevrConfigError for an unset ${NEVR_S5_SERVER_KEY:?} secret";
  } catch (const nevr::NevrConfigError& e) {
    EXPECT_NE(std::string(e.what()).find("NEVR_S5_SERVER_KEY must be set"),
              std::string::npos);
  }
}

// An unset bare ${VAR} keeps its text and does not fail the load (#137): on a client
// a throw would drop every plugin over one value. ${VAR:?msg} still throws.
TEST(NevrConfig, BareVarUnsetKeepsLiteralText) {
  UnsetEnv("NEVR_TEST_BARE");
  const nevr::NevrConfig cfg =
      nevr::NevrConfig::LoadFromString("services:\n  serverdb: \"pre-${NEVR_TEST_BARE}-post\"\n");
  EXPECT_EQ(cfg.GetString("services.serverdb").value_or(""), "pre-${NEVR_TEST_BARE}-post");
}

// One name per unset variable however often it is referenced: the load reports one entry per name, and the PC caller logs one warning per entry.
TEST(NevrConfig, UnsetBareVarIsReportedOncePerName) {
  UnsetEnv("NEVR_TEST_BARE_ONCE");
  UnsetEnv("NEVR_TEST_BARE_TWICE");
  const nevr::NevrConfig cfg = nevr::NevrConfig::LoadFromString(
      "services:\n  serverdb: \"${NEVR_TEST_BARE_ONCE}\"\n  loginservice: \"${NEVR_TEST_BARE_ONCE}/${NEVR_TEST_BARE_TWICE}\"\n");
  EXPECT_EQ(cfg.UnsetBareVars(), (std::vector<std::string>{"NEVR_TEST_BARE_ONCE", "NEVR_TEST_BARE_TWICE"}));
  SetEnv("NEVR_TEST_BARE_ONCE", "x");
  SetEnv("NEVR_TEST_BARE_TWICE", "y");
  EXPECT_TRUE(nevr::NevrConfig::LoadFromString("services:\n  serverdb: \"${NEVR_TEST_BARE_ONCE}\"\n").UnsetBareVars().empty());
  UnsetEnv("NEVR_TEST_BARE_ONCE");
  UnsetEnv("NEVR_TEST_BARE_TWICE");
}

TEST(NevrConfig, BareVarUnsetInPluginArgKeepsPlugins) {
  UnsetEnv("NEVR_TEST_BARE_ARG");
  const nevr::NevrConfig cfg = nevr::NevrConfig::LoadFromString(
      "plugins:\n  - name: alpha\n    args:\n      template: \"${NEVR_TEST_BARE_ARG}\"\n  - name: beta\n");
  ASSERT_EQ(cfg.Plugins().size(), 2u);
  EXPECT_EQ(cfg.Plugins()[0].args.at("template"), "${NEVR_TEST_BARE_ARG}");
}

TEST(NevrConfig, BareVarSetStillResolves) {
  SetEnv("NEVR_TEST_BARE_SET", "value");
  const nevr::NevrConfig cfg =
      nevr::NevrConfig::LoadFromString("services:\n  serverdb: \"${NEVR_TEST_BARE_SET}\"\n");
  EXPECT_EQ(cfg.GetString("services.serverdb").value_or(""), "value");
  UnsetEnv("NEVR_TEST_BARE_SET");
}

// $${ is a literal ${: no variable is looked up, so an unset one can't fail the load.
TEST(NevrConfig, EscapedDollarBraceIsLiteral) {
  UnsetEnv("NEVR_TEST_ESCAPED");
  const nevr::NevrConfig cfg =
      nevr::NevrConfig::LoadFromString("services:\n  serverdb: \"$${NEVR_TEST_ESCAPED}\"\n");
  EXPECT_EQ(cfg.GetString("services.serverdb").value_or(""), "${NEVR_TEST_ESCAPED}");
}

// An escaped ${ beside a real variable: only the real one is resolved.
TEST(NevrConfig, EscapedDollarBraceBesideVariable) {
  SetEnv("NEVR_TEST_REAL", "value");
  const nevr::NevrConfig cfg = nevr::NevrConfig::LoadFromString(
      "services:\n  serverdb: \"$${literal}-${NEVR_TEST_REAL}-$$plain\"\n");
  EXPECT_EQ(cfg.GetString("services.serverdb").value_or(""), "${literal}-value-$$plain");
  UnsetEnv("NEVR_TEST_REAL");
}

// Only the `$${` triple is special. In `$$${X}` the first `$` is plain and the
// following `$${` yields `${`, so the output is `$${X}` and X is never looked up.
TEST(NevrConfig, TripleDollarBraceYieldsDoubleDollarLiteral) {
  UnsetEnv("NEVR_TEST_TRIPLE");
  const nevr::NevrConfig cfg =
      nevr::NevrConfig::LoadFromString("services:\n  serverdb: \"$$${NEVR_TEST_TRIPLE}\"\n");
  EXPECT_EQ(cfg.GetString("services.serverdb").value_or(""), "$${NEVR_TEST_TRIPLE}");
}

// An unterminated `$${X` is still an escape: `${X`, no throw.
TEST(NevrConfig, EscapedUnterminatedDollarBraceIsLiteral) {
  const nevr::NevrConfig cfg =
      nevr::NevrConfig::LoadFromString("services:\n  serverdb: \"$${X\"\n");
  EXPECT_EQ(cfg.GetString("services.serverdb").value_or(""), "${X");
}

// The escape also covers the required-with-message syntax: no lookup, no throw.
TEST(NevrConfig, EscapedRequiredSyntaxIsLiteralAndDoesNotThrow) {
  UnsetEnv("NEVR_TEST_ESC_REQ");
  nevr::NevrConfig cfg;
  EXPECT_NO_THROW(cfg = nevr::NevrConfig::LoadFromString(
                      "services:\n  serverdb: \"$${NEVR_TEST_ESC_REQ:?msg}\"\n"));
  EXPECT_EQ(cfg.GetString("services.serverdb").value_or(""), "${NEVR_TEST_ESC_REQ:?msg}");
}

// A literal `$` directly before a variable's value: the unset variable's default is `$`.
TEST(NevrConfig, LiteralDollarBeforeValueViaDefault) {
  UnsetEnv("NEVR_TEST_NOT_SET");
  SetEnv("NEVR_TEST_REAL", "value");
  const nevr::NevrConfig cfg = nevr::NevrConfig::LoadFromString(
      "services:\n  serverdb: \"${NEVR_TEST_NOT_SET:-$}${NEVR_TEST_REAL}\"\n");
  EXPECT_EQ(cfg.GetString("services.serverdb").value_or(""), "$value");
  UnsetEnv("NEVR_TEST_REAL");
}

// `$$` not followed by `{` is two literal dollars.
TEST(NevrConfig, DoubleDollarWithoutBraceIsUnchanged) {
  const nevr::NevrConfig cfg =
      nevr::NevrConfig::LoadFromString("services:\n  serverdb: \"$$plain\"\n");
  EXPECT_EQ(cfg.GetString("services.serverdb").value_or(""), "$$plain");
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

TEST(NevrConfig, LoadFromFileOrFailReadsControlledScratchFile) {
  ASSERT_TRUE(EnsureScratchConfigDirectory());
  ScopedScratchFile file(ScratchConfigPath("controlled-config"));
  {
    std::ofstream output(file.path(), std::ios::binary);
    ASSERT_TRUE(output.is_open()) << "could not create scratch fixture " << file.path();
    output << "services:\n  serverdb: \"wss://controlled.example/nevr\"\n"
              "network:\n  upnp_port: 47291\n"
              "plugins:\n  - name: controlled_plugin\n    required: true\n";
    ASSERT_TRUE(output.good()) << "could not write scratch fixture " << file.path();
  }

  // Exercise the production policy wrapper rather than only LoadFromFile:
  // client mode must return the parsed config on success.
  const nevr::NevrConfig cfg = nevr::NevrConfig::LoadFromFileOrFail(file.path(), /*is_server=*/false);
  EXPECT_FALSE(cfg.Empty());
  EXPECT_EQ(cfg.GetString("services.serverdb").value_or(""), "wss://controlled.example/nevr");
  EXPECT_EQ(cfg.GetInt("network.upnp_port").value_or(0), 47291);
  ASSERT_EQ(cfg.Plugins().size(), 1u);
  EXPECT_EQ(cfg.Plugins()[0].file, "controlled_plugin.dll");
  EXPECT_TRUE(cfg.Plugins()[0].required);
}

TEST(NevrConfig, LoadFromFileOrFailMissingScratchFileClientReturnsEmpty) {
  ASSERT_TRUE(EnsureScratchConfigDirectory());
  ScopedScratchFile file(ScratchConfigPath("missing-config"));
  // Ensure the test drives the missing-file branch even after an interrupted run.
  std::remove(file.path().c_str());

  // Client mode on a missing file: warns (not fatal) and returns an empty config.
  const nevr::NevrConfig cfg = nevr::NevrConfig::LoadFromFileOrFail(file.path(), /*is_server=*/false);
  EXPECT_TRUE(cfg.Empty());
}

TEST(NevrConfig, RejectedFileDoesNotLogItsUnknownKey) {
  ASSERT_TRUE(EnsureScratchConfigDirectory());
  ScopedScratchFile file(ScratchConfigPath("rejected-config"));
  constexpr const char* kSecret = "secret-canary-584";
  {
    std::ofstream output(file.path(), std::ios::binary);
    ASSERT_TRUE(output.is_open());
    output << kSecret << ": value\n";
    ASSERT_TRUE(output.good());
  }

  testing::internal::CaptureStderr();
  const nevr::NevrConfig cfg = nevr::NevrConfig::LoadFromFileOrFail(file.path(), /*is_server=*/false);
  const std::string logged = testing::internal::GetCapturedStderr();
  EXPECT_TRUE(cfg.Empty());
  EXPECT_NE(logged.find("reason=unknown_top_level_key line=1 column=1"), std::string::npos) << logged;
  EXPECT_EQ(logged.find(kSecret), std::string::npos) << logged;

  g_configFatalMessage.clear();
  SetFatalErrorHandler(CaptureConfigFatal);
  const nevr::NevrConfig serverCfg = nevr::NevrConfig::LoadFromFileOrFail(file.path(), /*is_server=*/true);
  SetFatalErrorHandler(nullptr);
  EXPECT_TRUE(serverCfg.Empty());
  EXPECT_NE(g_configFatalMessage.find("reason=unknown_top_level_key line=1 column=1"), std::string::npos)
      << g_configFatalMessage;
  EXPECT_EQ(g_configFatalMessage.find(kSecret), std::string::npos) << g_configFatalMessage;
}

TEST(NevrConfig, AcceptsSocialFacadeSection) {
  const auto cfg = nevr::NevrConfig::LoadFromString("social:\n  facade: true\n");
  ASSERT_TRUE(cfg.GetBool("social.facade").has_value());
  EXPECT_TRUE(*cfg.GetBool("social.facade"));
}

// `diagnostics` is a top-level section (the Quest's hardware dump is `diagnostics.hardware_dump`, #573). The PC
// accepts it and reads nothing from it.
TEST(NevrConfig, AcceptsDiagnosticsSection) {
  const auto cfg = nevr::NevrConfig::LoadFromString("diagnostics:\n  hardware_dump: true\n");
  ASSERT_TRUE(cfg.GetBool("diagnostics.hardware_dump").has_value());
  EXPECT_TRUE(*cfg.GetBool("diagnostics.hardware_dump"));
  EXPECT_FALSE(cfg.Empty());
}

// #954: `diagnostics.frame_stats` is read by the PC frame-time probe (service_map.cpp, FrameStatsMode), so it is on the
// schema and a config that sets it has no unknown key.
TEST(NevrConfig, DiagnosticsFrameStatsIsAKnownKey) {
  const auto cfg = nevr::NevrConfig::LoadFromString("diagnostics:\n  frame_stats: perf\n");
  ASSERT_TRUE(cfg.GetString("diagnostics.frame_stats").has_value());
  EXPECT_EQ(cfg.UnknownKeyCount(), 0u);
  const std::vector<std::string> known = nevr::KnownConfigKeyPaths();
  EXPECT_NE(std::find(known.begin(), known.end(), "diagnostics.frame_stats"), known.end());
}

// `graphics` is a top-level section (the Quest's texture quality is `graphics.texture_quality`, #943). The PC accepts it and
// reads nothing from it, so one config.yaml can serve both builds; an unknown section is still refused.
TEST(NevrConfig, AcceptsGraphicsSection) {
  const auto cfg = nevr::NevrConfig::LoadFromString("graphics:\n  texture_quality: very_high\n");
  ASSERT_TRUE(cfg.GetString("graphics.texture_quality").has_value());
  EXPECT_EQ(*cfg.GetString("graphics.texture_quality"), "very_high");
  EXPECT_FALSE(cfg.Empty());
  EXPECT_THROW(nevr::NevrConfig::LoadFromString("graphic:\n  texture_quality: high\n"), nevr::NevrConfigError);
}

// Kind tells "set to something that is not a value" from "not set": the typed getters give nullopt for both.
TEST(NevrConfig, KindSeesEveryKindOfNodeTheGettersCannotRead) {
  using nevr::NodeKind;
  const auto cfg = nevr::NevrConfig::LoadFromString(
      "graphics:\n  scalar: ultra\n  empty_string: \"\"\n  novalue:\n  tilde: ~\n  mapping:\n    a: 1\n  empty_mapping: {}\n"
      "  list:\n    - a\n    - b\n  list_of_maps:\n    - k: v\n  empty_list: []\nx-flat: 5\n");
  EXPECT_EQ(cfg.Kind("graphics"), NodeKind::kMapping);
  EXPECT_EQ(cfg.Kind("graphics.scalar"), NodeKind::kScalar);
  EXPECT_EQ(cfg.Kind("graphics.empty_string"), NodeKind::kScalar);
  EXPECT_EQ(cfg.Kind("x-flat"), NodeKind::kScalar);
  EXPECT_EQ(cfg.Kind("graphics.novalue"), NodeKind::kNull);
  EXPECT_EQ(cfg.Kind("graphics.tilde"), NodeKind::kNull);
  EXPECT_EQ(cfg.Kind("graphics.mapping"), NodeKind::kMapping);
  EXPECT_EQ(cfg.Kind("graphics.empty_mapping"), NodeKind::kMapping);
  EXPECT_EQ(cfg.Kind("graphics.list"), NodeKind::kSequence);
  EXPECT_EQ(cfg.Kind("graphics.list_of_maps"), NodeKind::kSequence);
  EXPECT_EQ(cfg.Kind("graphics.empty_list"), NodeKind::kSequence);
  for (const char* absent : {"graphic", "graphics.other", "graphics.scalar.deeper", "x-flat.deeper", "novalue", "", "graphics."}) {
    EXPECT_EQ(cfg.Kind(absent), NodeKind::kAbsent) << absent;
  }
  // The getters read none of the non-scalar nodes: that is the gap Kind closes.
  for (const char* notAValue : {"graphics.novalue", "graphics.tilde", "graphics.mapping", "graphics.list", "graphics.empty_list"}) {
    EXPECT_FALSE(cfg.GetString(notAValue).has_value()) << notAValue;
  }
  EXPECT_EQ(nevr::NevrConfig().Kind("graphics"), NodeKind::kAbsent);
}

// MappingKeys lists the scalar keys of a mapping in file order, and nothing for anything that is not a mapping.
TEST(NevrConfig, MappingKeysListsTheKeysOfAMappingInFileOrder) {
  const auto cfg = nevr::NevrConfig::LoadFromString(
      "graphics:\n  zeta: 1\n  alpha:\n    nested: 1\n  Mixed_Case: x\n  list:\n    - a\n  scalar_section_below: 1\n"
      "diagnostics: on\nsocial:\n  - a\n");
  const std::vector<std::string> want = {"zeta", "alpha", "Mixed_Case", "list", "scalar_section_below"};
  EXPECT_EQ(cfg.MappingKeys("graphics"), want);
  EXPECT_EQ(cfg.MappingKeys("graphics.alpha"), std::vector<std::string>{"nested"});
  EXPECT_TRUE(cfg.MappingKeys("graphics.zeta").empty());   // a scalar
  EXPECT_TRUE(cfg.MappingKeys("graphics.list").empty());   // a sequence
  EXPECT_TRUE(cfg.MappingKeys("diagnostics").empty());     // a scalar section
  EXPECT_TRUE(cfg.MappingKeys("social").empty());          // a sequence section
  EXPECT_TRUE(cfg.MappingKeys("services").empty());        // absent
  EXPECT_TRUE(nevr::NevrConfig().MappingKeys("graphics").empty());
}

// A rejection carries a stable reason token, never the parser's text: that text echoes the file's own words
// (a key, a value), which the Quest's sentinel log must not repeat.
TEST(NevrConfig, ErrorsCarryAStableReasonToken) {
  const auto reasonOf = [](const std::string& yaml) {
    try {
      nevr::NevrConfig::LoadFromString(yaml);
    } catch (const nevr::NevrConfigError& e) {
      return std::string(nevr::ReasonToken(e.Kind()));
    }
    return std::string("no error");
  };
  UnsetEnv("NEVR_TEST_REASON_REQUIRED");
  EXPECT_EQ(reasonOf("services: [unterminated\n"), "yaml_syntax");
  EXPECT_EQ(reasonOf("- a\n- b\n"), "not_a_mapping");
  EXPECT_EQ(reasonOf("bogus_section:\n  x: 1\n"), "unknown_top_level_key");
  EXPECT_EQ(reasonOf("plugins: 3\n"), "bad_plugins");
  EXPECT_EQ(reasonOf("plugins:\n  - 3\n"), "bad_plugins");
  EXPECT_EQ(reasonOf("plugins:\n  - args: {}\n"), "bad_plugins");  // an entry with no name
  EXPECT_EQ(reasonOf("auth:\n  http_key: \"${NEVR_TEST_REASON_REQUIRED:?must be set}\"\n"), "unset_required_variable");
  EXPECT_EQ(reasonOf("x-a: &a [*a]\n"), "nesting_too_deep");
  EXPECT_EQ(reasonOf("services:\n  serverdb: \"x\"\n"), "no error");
  EXPECT_STREQ(nevr::ReasonToken(nevr::NevrConfigErrorKind::kOther), "invalid_config");
  try {
    nevr::NevrConfig::LoadFromFile(ScratchConfigPath("no-such-file"));
    FAIL() << "an unreadable file must throw";
  } catch (const nevr::NevrConfigError& e) {
    EXPECT_STREQ(nevr::ReasonToken(e.Kind()), "unreadable");
  }
}

// An alias can make the tree a cycle or expand it exponentially. The walks over it are bounded (kMaxConfigDepth,
// kMaxConfigNodes) and a file that exceeds a bound is rejected with its own token. Unbounded, the cycles below end
// the process (stack overflow in ValidateInterpolation) and the nested aliases run for seconds.
namespace {

std::string ReasonOf(const std::string& yaml) {
  try {
    nevr::NevrConfig::LoadFromString(yaml);
  } catch (const nevr::NevrConfigError& e) {
    return std::string(nevr::ReasonToken(e.Kind()));
  }
  return std::string("no error");
}

// `levels` mappings of ten references each to the level below: 10^levels visits when walked as a tree.
std::string NestedAliases(int levels) {
  std::string s = "x-a0: &a0 [\"lol\"]\n";
  for (int i = 1; i <= levels; ++i) {
    s += "x-a" + std::to_string(i) + ": &a" + std::to_string(i) + " [";
    for (int j = 0; j < 10; ++j) s += (j ? ",*a" : "*a") + std::to_string(i - 1);
    s += "]\n";
  }
  return s;
}

// `x-d: [[...[s]...]]` with `brackets` sequences: the scalar sits at depth brackets + 1.
std::string NestedSequences(std::size_t brackets) {
  return "x-d: " + std::string(brackets, '[') + "s" + std::string(brackets, ']') + "\n";
}

// A rejection takes at most kMaxConfigNodes visits; the unbounded walk over 8 nested levels ran for 10.8 s.
constexpr std::chrono::milliseconds kRejectionBudget{100};

std::chrono::milliseconds RejectionTime(const std::string& yaml) {
  const auto start = std::chrono::steady_clock::now();
  ReasonOf(yaml);
  return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start);
}

// The fixture directory follows the environment and names no machine: a Linux path becomes a Z: path,
// and without the variable the process's temporary directory is used.
TEST(NevrConfig, ScratchConfigDirectoryFollowsTheEnvironment) {
  const char* saved = std::getenv("NEVR_CONFIG_TEST_DIR");
  const std::string restore = saved == nullptr ? "" : saved;

  SetEnv("NEVR_CONFIG_TEST_DIR", "/some/scratch/dir");
  EXPECT_EQ(ScratchConfigDirectory(), "Z:\\some\\scratch\\dir\\");
  EXPECT_EQ(ScratchConfigPath("x").rfind("Z:\\some\\scratch\\dir\\x-", 0), 0u);

  UnsetEnv("NEVR_CONFIG_TEST_DIR");
  const std::string fallback = ScratchConfigDirectory();
  const std::string suffix = "\\nevr-config-tests\\";
  EXPECT_TRUE(fallback.size() > suffix.size() &&
              fallback.compare(fallback.size() - suffix.size(), suffix.size(), suffix) == 0)
      << fallback;

  SetEnv("NEVR_CONFIG_TEST_DIR", restore.c_str());
}

}  // namespace

TEST(NevrConfig, ASelfReferencingSequenceIsRejectedNotWalkedForever) {
  EXPECT_EQ(ReasonOf("x-a: &a [*a]\n"), "nesting_too_deep");
  EXPECT_LT(RejectionTime("x-a: &a [*a]\n"), kRejectionBudget);
}

TEST(NevrConfig, ASelfReferencingMapIsRejectedNotWalkedForever) {
  EXPECT_EQ(ReasonOf("x-a: &a {k: *a}\n"), "nesting_too_deep");
  EXPECT_LT(RejectionTime("x-a: &a {k: *a}\n"), kRejectionBudget);
}

TEST(NevrConfig, ASelfReferencingPluginArgsMapIsRejected) {
  EXPECT_EQ(ReasonOf("plugins:\n  - name: p\n    args: &a {k: *a}\n"), "nesting_too_deep");
}

TEST(NevrConfig, NestedAliasesThatExpandExponentiallyAreRejectedByTheNodeBudget) {
  EXPECT_EQ(ReasonOf(NestedAliases(8)), "too_many_nodes");
  EXPECT_LT(RejectionTime(NestedAliases(8)), kRejectionBudget);
}

TEST(NevrConfig, NestingAtTheDepthCapLoadsAndOneDeeperIsRejected) {
  EXPECT_EQ(ReasonOf(NestedSequences(nevr::kMaxConfigDepth - 1)), "no error");
  EXPECT_EQ(ReasonOf(NestedSequences(nevr::kMaxConfigDepth)), "nesting_too_deep");
}

TEST(NevrConfig, ADeepButLegalNestingStillLoads) {
  EXPECT_EQ(ReasonOf(NestedSequences(32)), "no error");
}

TEST(NevrConfig, AMuchDeeperNestingIsRejectedByTheParserOrTheCap) {
  const std::string reason = ReasonOf(NestedSequences(30000));
  EXPECT_TRUE(reason == "yaml_syntax" || reason == "nesting_too_deep") << reason;
}

TEST(NevrConfig, TheNodeBudgetIsExactlyKMaxConfigNodes) {
  // The root mapping and the sequence are two visits; each scalar is one.
  const auto sequenceOf = [](std::size_t scalars) {
    std::string s = "x-n: [";
    for (std::size_t i = 0; i < scalars; ++i) s += i == 0 ? "0" : ",0";
    return s + "]\n";
  };
  EXPECT_EQ(ReasonOf(sequenceOf(nevr::kMaxConfigNodes - 2)), "no error");
  EXPECT_EQ(ReasonOf(sequenceOf(nevr::kMaxConfigNodes - 1)), "too_many_nodes");
}

// The parser logs nothing (the Quest builds it without the PC logger); the PC's callers log the unset-variable
// warning, once per name, through LogUnsetBareVars.
TEST(NevrConfig, LoadingLogsNothingAndTheCallerLogsEachUnsetVariableOnce) {
  UnsetEnv("NEVR_TEST_BARE_LOG_A");
  UnsetEnv("NEVR_TEST_BARE_LOG_B");
  testing::internal::CaptureStderr();
  const nevr::NevrConfig cfg = nevr::NevrConfig::LoadFromString(
      "services:\n  serverdb: \"${NEVR_TEST_BARE_LOG_A}\"\n  loginservice: \"${NEVR_TEST_BARE_LOG_A}/${NEVR_TEST_BARE_LOG_B}\"\n");
  const std::string duringLoad = testing::internal::GetCapturedStderr();
  EXPECT_EQ(duringLoad, "");

  testing::internal::CaptureStderr();
  nevr::LogUnsetBareVars(cfg);
  const std::string logged = testing::internal::GetCapturedStderr();
  const auto occurrences = [&logged](const std::string& needle) {
    std::size_t n = 0;
    for (std::size_t at = logged.find(needle); at != std::string::npos; at = logged.find(needle, at + 1)) ++n;
    return n;
  };
  EXPECT_EQ(occurrences("[NEVR.CONFIG] environment variable NEVR_TEST_BARE_LOG_A is not set"), 1u) << logged;
  EXPECT_EQ(occurrences("[NEVR.CONFIG] environment variable NEVR_TEST_BARE_LOG_B is not set"), 1u) << logged;
  EXPECT_NE(logged.find("kept as written"), std::string::npos) << logged;

  // Nothing unset, nothing logged.
  SetEnv("NEVR_TEST_BARE_LOG_A", "x");
  const nevr::NevrConfig set = nevr::NevrConfig::LoadFromString("services:\n  serverdb: \"${NEVR_TEST_BARE_LOG_A}\"\n");
  testing::internal::CaptureStderr();
  nevr::LogUnsetBareVars(set);
  EXPECT_EQ(testing::internal::GetCapturedStderr(), "");
  UnsetEnv("NEVR_TEST_BARE_LOG_A");
}

// The policy wrapper keeps logging the same warning when it loads a file (it is the PC caller).
TEST(NevrConfig, LoadFromFileOrFailLogsTheUnsetVariableWarning) {
  ASSERT_TRUE(EnsureScratchConfigDirectory());
  ScopedScratchFile file(ScratchConfigPath("unset-var-config"));
  {
    std::ofstream output(file.path(), std::ios::binary);
    ASSERT_TRUE(output.is_open());
    output << "services:\n  serverdb: \"${NEVR_TEST_BARE_LOG_C}\"\n";
  }
  UnsetEnv("NEVR_TEST_BARE_LOG_C");
  testing::internal::CaptureStderr();
  const nevr::NevrConfig cfg = nevr::NevrConfig::LoadFromFileOrFail(file.path(), /*is_server=*/false);
  const std::string logged = testing::internal::GetCapturedStderr();
  EXPECT_FALSE(cfg.Empty());
  EXPECT_NE(logged.find("[NEVR.CONFIG] environment variable NEVR_TEST_BARE_LOG_C is not set"), std::string::npos) << logged;
}

// #583: a key that repeats in one mapping is counted, and the first value is the one read (pinned: it is what yaml-cpp
// gives and what the Quest reads too).
TEST(NevrConfig, DuplicateKeysAreCountedAndTheFirstValueIsRead) {
  const nevr::NevrConfig cfg = nevr::NevrConfig::LoadFromString(
      "services:\n  socket_uri: \"wss://first.example/x\"\n  socket_uri: \"wss://second.example/x\"\n"
      "social:\n  facade: true\n  facade: false\n");
  EXPECT_EQ(cfg.DuplicateKeyCount(), 2u);
  EXPECT_EQ(cfg.UnknownKeyCount(), 0u);
  EXPECT_EQ(cfg.GetString("services.socket_uri").value_or(""), "wss://first.example/x");
  EXPECT_EQ(cfg.GetBool("social.facade").value_or(false), true);
}

// #583: a key under a known section that no reader reads is counted; known keys, x- extensions and a free-form
// section (profiles) are not.
TEST(NevrConfig, UnknownKeysUnderKnownSectionsAreCounted) {
  const nevr::NevrConfig cfg = nevr::NevrConfig::LoadFromString(
      "social:\n  send_presence_to_meta: false\n  send_presense_to_meta: false\n  x-note: 1\n"
      "services:\n  socket_uri: \"wss://a.example/x\"\n  loginservice_host: \"wss://b.example/x\"\n"
      "profiles:\n  anything_goes: 1\n");
  EXPECT_EQ(cfg.UnknownKeyCount(), 2u);
  EXPECT_EQ(cfg.DuplicateKeyCount(), 0u);
}

// #1156: network.invite_codes is on the schema, so the key that turns the invite feature on is never flagged as unknown.
TEST(NevrConfig, TheInviteCodesKeyIsOnTheSchema) {
  const nevr::NevrConfig cfg = nevr::NevrConfig::LoadFromString("network:\n  invite_codes: true\n");
  EXPECT_EQ(cfg.UnknownKeyCount(), 0u);
  EXPECT_EQ(cfg.GetBool("network.invite_codes").value_or(false), true);
}

TEST(NevrConfig, ACleanFileCountsNothing) {
  const nevr::NevrConfig cfg = nevr::NevrConfig::LoadFromString(
      "version: 1\nauth:\n  password: \"x\"\nnetwork:\n  regions: \"us-west\"\n");
  EXPECT_EQ(cfg.UnknownKeyCount(), 0u);
  EXPECT_EQ(cfg.DuplicateKeyCount(), 0u);
}

// #583: the PC's one Warning per load names the counts and no key or value.
TEST(NevrConfig, ShapeWarningCarriesCountsOnly) {
  ASSERT_TRUE(EnsureScratchConfigDirectory());
  ScopedScratchFile file(ScratchConfigPath("shape-warning-config"));
  {
    std::ofstream output(file.path(), std::ios::binary);
    ASSERT_TRUE(output.is_open());
    output << "social:\n  facade: true\n  facade: false\n  send_presense: false\n";
  }
  testing::internal::CaptureStderr();
  const nevr::NevrConfig cfg = nevr::NevrConfig::LoadFromFileOrFail(file.path(), /*is_server=*/false);
  const std::string logged = testing::internal::GetCapturedStderr();
  EXPECT_FALSE(cfg.Empty());
  EXPECT_NE(logged.find("[NEVR.CONFIG] config.yaml has duplicates=1 unknown_nested=1"), std::string::npos) << logged;
  EXPECT_EQ(logged.find("send_presense"), std::string::npos) << logged;
  EXPECT_EQ(logged.find("facade"), std::string::npos) << logged;
}

// #583: every key the Quest reads is on the schema, so the unknown-key count never flags a key a build uses. The
// PC's keys are checked against the same list in test_service_map (MigratedYamlPaths).
TEST(NevrConfig, KnownPathsCoverEveryKeyTheQuestReads) {
  const std::vector<std::string> known = nevr::KnownConfigKeyPaths();
  const std::vector<std::string> questReads = {
      "services.socket_uri", "auth.http_uri",          "auth.http_key",          "auth.server_key",
      "social.facade",       "social.send_presence_to_meta", "diagnostics.hardware_dump",
      "diagnostics.ui_event_probe", "diagnostics.pnsrad_provider", "diagnostics.clock_levels_override",
      "graphics.texture_quality"};
  for (const std::string& path : questReads) {
    EXPECT_NE(std::find(known.begin(), known.end(), path), known.end()) << path << " is read but not on the schema";
  }
}

// #1857 — the reader facts an in-game plugin manager relies on. A manager lists every configured entry, in file order,
// with its enabled flag, so the reader must keep a disabled entry in place and must not drop or reorder entries.
TEST(NevrConfigPlugins, DisabledEntryKeepsItsPlaceAndFlags) {
  const nevr::NevrConfig cfg = nevr::NevrConfig::LoadFromString(
      "plugins:\n"
      "  - name: first\n"
      "  - name: second\n"
      "    enabled: false\n"
      "    required: false\n"
      "    early: true\n"
      "    target: server\n"
      "  - name: third\n");
  const std::vector<nevr::PluginSpec>& plugins = cfg.Plugins();
  ASSERT_EQ(plugins.size(), 3u);
  EXPECT_EQ(plugins[0].name, "first");
  EXPECT_EQ(plugins[1].name, "second");
  EXPECT_EQ(plugins[2].name, "third");
  EXPECT_TRUE(plugins[0].enabled);
  EXPECT_FALSE(plugins[1].enabled);
  EXPECT_FALSE(plugins[1].required);
  EXPECT_TRUE(plugins[1].early);
  EXPECT_EQ(plugins[1].target, "server");
  EXPECT_TRUE(plugins[2].enabled);
}

// File order is the order of the list, not an alphabetical one, and a repeated name is kept as written (two entries).
TEST(NevrConfigPlugins, ListOrderIsFileOrderAndRepeatedNamesAreKept) {
  const nevr::NevrConfig cfg = nevr::NevrConfig::LoadFromString(
      "plugins:\n  - name: zeta\n  - name: alpha\n  - name: zeta\n    file: other.dll\n");
  const std::vector<nevr::PluginSpec>& plugins = cfg.Plugins();
  ASSERT_EQ(plugins.size(), 3u);
  EXPECT_EQ(plugins[0].name, "zeta");
  EXPECT_EQ(plugins[1].name, "alpha");
  EXPECT_EQ(plugins[2].name, "zeta");
  EXPECT_EQ(plugins[0].file, "zeta.dll");
  EXPECT_EQ(plugins[2].file, "other.dll");
}

// An `enabled` (or `required`, `early`) value that is not a recognised boolean falls back to the field's default, so a typo
// reads as enabled. A manager that shows state must show what the loader will do, which is this.
TEST(NevrConfigPlugins, UnrecognisedBooleanFallsBackToTheDefault) {
  const nevr::NevrConfig cfg = nevr::NevrConfig::LoadFromString(
      "plugins:\n  - name: typo\n    enabled: maybe\n    required: maybe\n    early: maybe\n");
  ASSERT_EQ(cfg.Plugins().size(), 1u);
  EXPECT_TRUE(cfg.Plugins()[0].enabled);
  EXPECT_FALSE(cfg.Plugins()[0].required);
  EXPECT_FALSE(cfg.Plugins()[0].early);
}

// `args` is kept per entry (a key shared by two entries stays separate), and a value that is not a mapping is ignored.
TEST(NevrConfigPlugins, ArgsStayWithTheirEntryAndANonMappingIsIgnored) {
  const nevr::NevrConfig cfg = nevr::NevrConfig::LoadFromString(
      "plugins:\n"
      "  - name: a\n"
      "    args: {level: 1}\n"
      "  - name: b\n"
      "    args: {level: 2}\n"
      "  - name: c\n"
      "    args: 3\n");
  const std::vector<nevr::PluginSpec>& plugins = cfg.Plugins();
  ASSERT_EQ(plugins.size(), 3u);
  EXPECT_EQ(plugins[0].args.at("level"), "1");
  EXPECT_EQ(plugins[1].args.at("level"), "2");
  EXPECT_TRUE(plugins[2].args.empty());
}

// Malformed plugin entries are refused with the stable token, whole-file: the reader never returns a half list.
TEST(NevrConfigPlugins, MalformedEntriesAreRefused) {
  const auto reasonOf = [](const std::string& yaml) {
    try {
      nevr::NevrConfig::LoadFromString(yaml);
    } catch (const nevr::NevrConfigError& e) {
      return std::string(nevr::ReasonToken(e.Kind()));
    }
    return std::string("no error");
  };
  EXPECT_EQ(reasonOf("plugins:\n  - name: \"\"\n"), "bad_plugins");                   // empty name
  EXPECT_EQ(reasonOf("plugins:\n  - name: [a, b]\n"), "bad_plugins");                 // name is not a scalar
  EXPECT_EQ(reasonOf("plugins:\n  - name: ok\n  - 3\n"), "bad_plugins");              // a later entry is bad
  EXPECT_EQ(reasonOf("plugins: {name: ok}\n"), "bad_plugins");                        // a mapping, not a list
  EXPECT_EQ(reasonOf("plugins:\n  - name: ok\n"), "no error");
}

}  // namespace
