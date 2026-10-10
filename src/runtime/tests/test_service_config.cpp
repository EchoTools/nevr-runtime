#include "runtime/tests/service_config_test_hooks.h"

#include <gtest/gtest.h>

#include <string>

#include <windows.h>

#include <nlohmann/json.hpp>

#include "extension/module_interface.h"
#include "runtime/lifecycle/service_config.h"
#include "runtime/lifecycle/config_redirect_result.h"

BOOL g_isServer = FALSE;
CHAR g_customConfigPath[MAX_PATH] = {};

namespace {

using nevr_runtime::lifecycle::test::FailInternAtAccessor;
using nevr_runtime::lifecycle::test::ResetAccessorInputs;
using nevr_runtime::lifecycle::test::SetAccessorInputs;
using nevr_runtime::lifecycle::test::ThrowAtAccessor;

constexpr const char* kConfig = R"YAML(
version: "1"
services:
  login: "wss://login.example/nevr"
  matchmaking: "wss://match.example/nevr"
  socket_uri: "ws://service.example:80/spr"
auth:
  http_uri: "https://service.example:7350"
  server_key: "secret-test-key"
guilds: ["alpha", "beta"]
)YAML";

const nevr::NevrConfig& Config() {
  static const nevr::NevrConfig config = nevr::NevrConfig::LoadFromString(kConfig);
  return config;
}

const nevr::NevrConfig& EmptyConfig() {
  static const nevr::NevrConfig config = nevr::NevrConfig::LoadFromString("version: \"1\"\n");
  return config;
}

const nevr_cfg::FlatDefaults& NoDefaults() {
  static const nevr_cfg::FlatDefaults defaults;
  return defaults;
}

const nevr_cfg::FlatDefaults& SocketDefault() {
  static const nevr_cfg::FlatDefaults defaults = {
      {"nevr_socket_uri", "ws://default.example:80/spr"},
  };
  return defaults;
}

void SetConfigInputs(bool serverMode = false) {
  ResetAccessorInputs();
  SetAccessorInputs(&Config(), &NoDefaults(), serverMode);
}

void SetEmptyInputs() {
  ResetAccessorInputs();
  SetAccessorInputs(&EmptyConfig(), &NoDefaults(), false);
}

enum class Accessor {
  kGetFlat,
  kGetFlatCsv,
  kServiceHost,
  kRedirect,
  kGameNativeDefault,
  kAutoRelay,
  kGameNativeConfigJson,
  kModuleConfigGet,
};

struct AccessorCase {
  const char* internAccessor;
  Accessor accessor;
};

constexpr AccessorCase kAccessors[] = {
    {"NevrCfgGetFlat", Accessor::kGetFlat},
    {"NevrCfgGetFlatCsv", Accessor::kGetFlatCsv},
    {"NevrCfgServiceHost", Accessor::kServiceHost},
    {"NevrCfgRedirect", Accessor::kRedirect},
    {"NevrGameNativeDefault", Accessor::kGameNativeDefault},
    {"NevrCfgAutoRelay", Accessor::kAutoRelay},
    {"NevrCfgGameNativeConfigJson", Accessor::kGameNativeConfigJson},
    {"NevrCfgGetFlat", Accessor::kModuleConfigGet},
};

const char* InvokeAccessor(Accessor accessor) {
  switch (accessor) {
    case Accessor::kGetFlat: return NevrCfgGetFlat("nevr_server_key");
    case Accessor::kGetFlatCsv: return NevrCfgGetFlatCsv("nevr_guilds");
    case Accessor::kServiceHost: {
      int source = -1;
      return NevrCfgServiceHost("matchingservice_host", &source);
    }
    case Accessor::kRedirect:
      return NevrCfgRedirect("wss://login.readyatdawn.com/rad15", nullptr, 0, 0);
    case Accessor::kGameNativeDefault: return NevrGameNativeDefault("publisher_lock");
    case Accessor::kAutoRelay: return NevrCfgAutoRelay(53748);
    case Accessor::kGameNativeConfigJson: return NevrCfgGameNativeConfigJson();
    case Accessor::kModuleConfigGet: {
      NvrModuleContext context{};
      context.config_get = &NevrCfgGetFlat;
      return context.config_get("nevr_socket_uri");
    }
  }
  return nullptr;
}

}  // namespace

// Minimal test boundary stubs: no file, environment, game, or service access.
void ForceFatalExit(unsigned int code) { ExitProcess(code); }

void ServerFatal(const CHAR*, ...) { ExitProcess(3); }

TEST(StableStringPoolAccessors, InjectedConfigAndDefaultsNeverDiscoverFilesOrReadEnvironment) {
  ResetAccessorInputs();
  SetAccessorInputs(&EmptyConfig(), &SocketDefault(), false);
  EXPECT_STREQ(NevrCfgGetFlat("nevr_socket_uri"), "ws://default.example:80/spr");

  SetConfigInputs();
  const char* socket = NevrCfgGetFlat("nevr_socket_uri");
  ASSERT_NE(socket, nullptr);
  EXPECT_STREQ(socket, "ws://service.example:80/spr");
  EXPECT_EQ(NevrCfgGetFlat("nevr_socket_uri"), socket);

  const char* csv = NevrCfgGetFlatCsv("nevr_guilds");
  ASSERT_NE(csv, nullptr);
  EXPECT_STREQ(csv, "alpha,beta");

  int source = -1;
  const char* primary = NevrCfgServiceHost("matchingservice_host", &source);
  ASSERT_NE(primary, nullptr);
  EXPECT_EQ(source, 0);
  EXPECT_STREQ(primary, "wss://match.example/nevr");

  source = -1;
  const char* fallback = NevrCfgServiceHost("serverdb_host", &source);
  ASSERT_NE(fallback, nullptr);
  EXPECT_EQ(source, 1);
  EXPECT_STREQ(fallback, "wss://login.example/nevr");

  const char* redirected = NevrCfgRedirect("wss://login.readyatdawn.com/rad15", nullptr, 1, 53748);
  ASSERT_NE(redirected, nullptr);
  EXPECT_STREQ(redirected, "ws://127.0.0.1:53748");
  EXPECT_EQ(NevrCfgRedirect("wss://login.readyatdawn.com/rad15", nullptr, 1, 53748), redirected);
  EXPECT_STREQ(redirected, "ws://127.0.0.1:53748");  // later read after further interning

  EXPECT_STREQ(NevrGameNativeDefault("publisher_lock"), "echotools");
  EXPECT_STREQ(NevrCfgAutoRelay(53748), "ws://127.0.0.1:53748");

  const char* json = NevrCfgGameNativeConfigJson();
  ASSERT_NE(json, nullptr);
  const nlohmann::json document = nlohmann::json::parse(json);
  EXPECT_EQ(document["social_plugin"]["server_key"], "secret-test-key");

  NvrModuleContext context{};
  context.config_get = &NevrCfgGetFlat;
  EXPECT_EQ(context.config_get("nevr_socket_uri"), socket);
}

TEST(StableStringPoolAccessors, AdapterProviderDetachLeavesEscapedValueReadable) {
  const char* escaped = nullptr;
  {
    const nevr::NevrConfig config = nevr::NevrConfig::LoadFromString(kConfig);
    const nevr_cfg::FlatDefaults defaults;
    ResetAccessorInputs();
    SetAccessorInputs(&config, &defaults, false);
    escaped = NevrCfgGetFlat("nevr_socket_uri");
    ASSERT_NE(escaped, nullptr);
    ResetAccessorInputs();  // Detach the test adapter from its borrowed config/default providers.
  }
  EXPECT_STREQ(escaped, "ws://service.example:80/spr");
}

TEST(StableStringPoolAccessors, AbsentServiceHostPublishesSourceTwoAndAllowsNullOutPointer) {
  SetEmptyInputs();
  int source = 0;
  EXPECT_EQ(NevrCfgServiceHost("matchingservice_host", &source), nullptr);
  EXPECT_EQ(source, 2);
  source = 1;
  EXPECT_EQ(NevrCfgServiceHost("serverdb_host", &source), nullptr);
  EXPECT_EQ(source, 2);
  EXPECT_EQ(NevrCfgServiceHost("serverdb_host", nullptr), nullptr);
}

TEST(StableStringPoolAccessors, UnchangedRedirectPreservesExactGameDefaultPointer) {
  const char defaultValue[] = "https://unrelated.example/path";
  const char* gameResult = defaultValue;
  EXPECT_EQ(nevr::lifecycle::ChooseRedirectedOrOriginal(gameResult, nullptr), defaultValue);
  EXPECT_EQ(nevr::lifecycle::ChooseRedirectedOrOriginal(gameResult, nullptr), gameResult);
}

TEST(StableStringPoolAccessors, EveryCAccessorTerminatesOnInternFailureWithRedactedDiagnostic) {
  for (const AccessorCase& item : kAccessors) {
    const std::string expected =
        "^\\[NEVR\\.CONFIG\\] C accessor failed accessor=" + std::string(item.internAccessor) +
        " status=allocation_failure strings=7 live_bytes=13\\r?\\n$";
    ASSERT_EXIT(
        {
          SetConfigInputs();
          FailInternAtAccessor(item.internAccessor);
          (void)InvokeAccessor(item.accessor);
          ExitProcess(0);
        },
        ::testing::ExitedWithCode(1), expected);
  }
}

TEST(StableStringPoolAccessors, EveryCAccessorContainsStdExceptionWithRedactedDiagnostic) {
  for (const AccessorCase& item : kAccessors) {
    const std::string expected =
        "^\\[NEVR\\.CONFIG\\] C accessor failed accessor=" + std::string(item.internAccessor) +
        " status=exception strings=0 live_bytes=0\\r?\\n$";
    ASSERT_EXIT(
        {
          SetConfigInputs();
          ThrowAtAccessor(item.internAccessor);
          (void)InvokeAccessor(item.accessor);
          ExitProcess(0);
        },
        ::testing::ExitedWithCode(1), expected);
  }
}
