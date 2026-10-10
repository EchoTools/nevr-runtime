#include "runtime/tests/service_config_test_hooks.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <string>

#include <windows.h>

#include <nlohmann/json.hpp>

#include "extension/module_interface.h"
#include "runtime/lifecycle/service_config.h"
#include "runtime/lifecycle/stable_string_pool.h"
#include "runtime/lifecycle/config_redirect_result.h"

BOOL g_isServer = FALSE;
CHAR g_customConfigPath[MAX_PATH] = {};

namespace {

using nevr::lifecycle::test::FailInternAtAccessor;
using nevr::lifecycle::test::ResetAccessorInputs;
using nevr::lifecycle::test::SetAccessorInputs;
using nevr::lifecycle::test::ThrowAtAccessor;

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

// The built-in defaults are a client convenience: a dedicated server is configured explicitly
// and must not start a bridge or authenticate with an embedded key. The accessors' injected
// defaults pass through the same SelectBuiltinDefaults gate as the embedded ones, so a
// server-mode run with only built-in defaults finds nothing. (That production passes
// IsServerMode() into the gate is pinned by tools/tests/test_builtin_defaults_contract.py.)
TEST(ServerModeDefaultsGate, ClientModeSeesTheEmbeddedDefault) {
  ResetAccessorInputs();
  SetAccessorInputs(&EmptyConfig(), &SocketDefault(), false);
  ASSERT_NE(NevrCfgGetFlat("nevr_socket_uri"), nullptr);
  EXPECT_STREQ(NevrCfgGetFlat("nevr_socket_uri"), "ws://default.example:80/spr");
  EXPECT_NE(NevrCfgAutoRelay(53748), nullptr);
}

TEST(ServerModeDefaultsGate, ServerModeDoesNotSeeTheEmbeddedDefault) {
  ResetAccessorInputs();
  SetAccessorInputs(&EmptyConfig(), &SocketDefault(), true);
  EXPECT_EQ(NevrCfgGetFlat("nevr_socket_uri"), nullptr);
  EXPECT_EQ(NevrCfgAutoRelay(53748), nullptr);
  EXPECT_EQ(NevrCfgRedirect("wss://login.readyatdawn.com/rad15", nullptr, 0, 0), nullptr);
}

TEST(ServerModeDefaultsGate, ServerModeStillReadsAnExplicitConfigValue) {
  ResetAccessorInputs();
  SetAccessorInputs(&Config(), &SocketDefault(), true);
  const char* value = NevrCfgGetFlat("nevr_server_key");
  ASSERT_NE(value, nullptr);
  EXPECT_STREQ(value, "secret-test-key");
}

TEST(ServerModeDefaultsGate, ServerModeSuppliesNoGameNativeConfig) {
  ResetAccessorInputs();
  SetAccessorInputs(&Config(), &NoDefaults(), true);
  EXPECT_EQ(NevrCfgGameNativeConfigJson(), nullptr);
  SetAccessorInputs(&Config(), &NoDefaults(), false);
  EXPECT_NE(NevrCfgGameNativeConfigJson(), nullptr);
}

TEST(StableStringPoolAccessors, UnchangedRedirectPreservesExactGameDefaultPointer) {
  const char defaultValue[] = "https://unrelated.example/path";
  const char* gameResult = defaultValue;
  EXPECT_EQ(nevr::lifecycle::ChooseRedirectedOrOriginal(gameResult, nullptr), defaultValue);
  EXPECT_EQ(nevr::lifecycle::ChooseRedirectedOrOriginal(gameResult, nullptr), gameResult);
}

// DecideServiceRedirect is the decision RedirectServiceUrl (config.cpp) makes for every string the
// game reads from its JSON config. The tests drive it with counting callbacks and with callbacks
// that call the real accessors over injected config. config.cpp itself links into no test; the
// arguments it passes (the armed flag, the bridge state and port) are pinned by
// tools/tests/test_quest_shared_redirect_sources.py.
struct CallCounter {
  int httpTarget = 0;
  int redirect = 0;
};

TEST(DecideServiceRedirect, NothingIsLookedUpBeforeTheRedirectsAreArmed) {
  CallCounter calls;
  const char gameResult[] = "wss://login.readyatdawn.com/rad15";
  const char* chosen = nevr::lifecycle::DecideServiceRedirect(
      false, "loginservice_host", gameResult, [&] { ++calls.httpTarget; return nullptr; },
      [&](const char*, const char*) { ++calls.redirect; return nullptr; });
  EXPECT_EQ(chosen, gameResult);
  EXPECT_EQ(calls.httpTarget, 0);
  EXPECT_EQ(calls.redirect, 0);
}

// A redirect that hands back the very pointer the game passed in is "no change": the caller
// compares pointers and skips its log line, so it neither logs a from=X to=X pair nor hides a real one.
TEST(DecideServiceRedirect, ARedirectReturningTheGamesOwnPointerIsNoChange) {
  const char gameResult[] = "wss://login.readyatdawn.com/rad15";
  const char* chosen = nevr::lifecycle::DecideServiceRedirect(
      true, "loginservice_host", gameResult, [] { return nullptr; },
      [&](const char* r, const char*) { return r; });
  EXPECT_EQ(chosen, gameResult);
}

TEST(DecideServiceRedirect, ANullResultOrKeyIsReturnedUntouchedWithoutLookups) {
  CallCounter calls;
  auto http = [&] { ++calls.httpTarget; return nullptr; };
  auto redirect = [&](const char*, const char*) { ++calls.redirect; return nullptr; };
  EXPECT_EQ(nevr::lifecycle::DecideServiceRedirect(true, "loginservice_host", nullptr, http, redirect), nullptr);
  const char gameResult[] = "wss://login.readyatdawn.com/rad15";
  EXPECT_EQ(nevr::lifecycle::DecideServiceRedirect(true, nullptr, gameResult, http, redirect), gameResult);
  EXPECT_EQ(calls.httpTarget, 0);
  EXPECT_EQ(calls.redirect, 0);
}

TEST(DecideServiceRedirect, ArmedHandsTheResultAndTheHttpTargetToTheRedirect) {
  CallCounter calls;
  const char gameResult[] = "https://api.readyatdawn.com/x";
  const char target[] = "https://service.example:7350";
  const char replacement[] = "https://service.example:7350";
  const char* seenResult = nullptr;
  const char* seenTarget = nullptr;
  const char* chosen = nevr::lifecycle::DecideServiceRedirect(
      true, "apiservice_host", gameResult, [&] { ++calls.httpTarget; return target; },
      [&](const char* r, const char* t) { ++calls.redirect; seenResult = r; seenTarget = t; return replacement; });
  EXPECT_EQ(chosen, replacement);
  EXPECT_EQ(seenResult, gameResult);
  EXPECT_EQ(seenTarget, target);
  EXPECT_EQ(calls.httpTarget, 1);
  EXPECT_EQ(calls.redirect, 1);
}

TEST(DecideServiceRedirect, NoRedirectKeepsTheGamesExactPointer) {
  SetConfigInputs();
  const char gameResult[] = "https://unrelated.example/path";
  const char* chosen = nevr::lifecycle::DecideServiceRedirect(
      true, "somekey", gameResult, [] { return NevrCfgGetFlat("nevr_http_uri"); },
      [](const char* r, const char* t) { return NevrCfgRedirect(r, t, 0, 0); });
  EXPECT_EQ(chosen, gameResult);
}

TEST(DecideServiceRedirect, ArmedWithTheRealAccessorsRedirectsAReadyAtDawnSocket) {
  SetConfigInputs();
  const char gameResult[] = "wss://login.readyatdawn.com/rad15";
  const char* chosen = nevr::lifecycle::DecideServiceRedirect(
      true, "loginservice_host", gameResult, [] { return NevrCfgGetFlat("nevr_http_uri"); },
      [](const char* r, const char* t) { return NevrCfgRedirect(r, t, 0, 0); });
  ASSERT_NE(chosen, gameResult);
  EXPECT_STREQ(chosen, "ws://service.example:80/spr");
}

TEST(DecideServiceRedirect, ABridgeRewritesTheSocketToLoopbackAndTheHttpTargetNeverUsesIt) {
  SetConfigInputs();
  const char socketResult[] = "wss://login.readyatdawn.com/rad15";
  const char* viaBridge = nevr::lifecycle::DecideServiceRedirect(
      true, "loginservice_host", socketResult, [] { return NevrCfgGetFlat("nevr_http_uri"); },
      [](const char* r, const char* t) { return NevrCfgRedirect(r, t, 1, 53748); });
  EXPECT_STREQ(viaBridge, "ws://127.0.0.1:53748");

  const char httpResult[] = "https://api.readyatdawn.com/x";
  const char* http = nevr::lifecycle::DecideServiceRedirect(
      true, "apiservice_host", httpResult, [] { return NevrCfgGetFlat("nevr_http_uri"); },
      [](const char* r, const char* t) { return NevrCfgRedirect(r, t, 1, 53748); });
  EXPECT_STREQ(http, "https://service.example:7350");
}

TEST(DecideUnconfiguredApiRedirect, TheGamesDefaultApiHostGoesToTheConfiguredHttpService) {
  SetConfigInputs();
  const auto http = [] { return NevrCfgGetFlat("nevr_http_uri"); };
  const auto redirect = [](const char* r, const char* t) { return NevrCfgRedirect(r, t, 0, 0); };
  const char api[] = "https://api.readyatdawn.com";
  const char* chosen = nevr::lifecycle::DecideUnconfiguredApiRedirect(true, api, http, redirect);
  ASSERT_NE(chosen, api);
  EXPECT_STREQ(chosen, "https://service.example:7350");
  // The per-environment form of the same host.
  const char env[] = "https://api-dev.readyatdawn.com";
  const char* chosenEnv = nevr::lifecycle::DecideUnconfiguredApiRedirect(true, env, http, redirect);
  EXPECT_STREQ(chosenEnv, "https://service.example:7350");
}

TEST(DecideUnconfiguredApiRedirect, OnlyTheTwoRealApiPrefixesMatch) {
  EXPECT_TRUE(nevr::lifecycle::IsGameApiBaseUrl("https://api.readyatdawn.com"));
  EXPECT_TRUE(nevr::lifecycle::IsGameApiBaseUrl("https://api-dev.readyatdawn.com"));
  EXPECT_FALSE(nevr::lifecycle::IsGameApiBaseUrl("https://apiary.example"));
  EXPECT_FALSE(nevr::lifecycle::IsGameApiBaseUrl("https://api"));
  EXPECT_FALSE(nevr::lifecycle::IsGameApiBaseUrl("http://api.readyatdawn.com"));
  EXPECT_FALSE(nevr::lifecycle::IsGameApiBaseUrl("https://login.readyatdawn.com"));
  EXPECT_FALSE(nevr::lifecycle::IsGameApiBaseUrl(nullptr));
}

// HttpConnectHook's decision: a configured host (apiservice_host, loginservice_host, api_host) wins and
// the nevr_http_uri fallback is not even looked up; only an untouched game pointer reaches the fallback.
TEST(DecideHttpConnectUri, AConfiguredHostWinsAndTheFallbackIsNotConsulted) {
  SetConfigInputs();
  int httpLookups = 0;
  int redirects = 0;
  const auto http = [&] { ++httpLookups; return NevrCfgGetFlat("nevr_http_uri"); };
  const auto redirect = [&](const char* r, const char* t) { ++redirects; return NevrCfgRedirect(r, t, 0, 0); };
  const char game[] = "https://api.readyatdawn.com";
  const char configured[] = "https://api.configured.example";
  EXPECT_EQ(nevr::lifecycle::DecideHttpConnectUri(true, game, configured, http, redirect), configured);
  EXPECT_EQ(httpLookups, 0);
  EXPECT_EQ(redirects, 0);
  // The chain left the game's pointer: the unconfigured fallback applies.
  const char* fallback = nevr::lifecycle::DecideHttpConnectUri(true, game, game, http, redirect);
  EXPECT_STREQ(fallback, "https://service.example:7350");
  EXPECT_EQ(httpLookups, 1);
  // A host that is neither the game's API base nor its queue's graph host is left alone either way.
  const char other[] = "https://login.readyatdawn.com";
  EXPECT_EQ(nevr::lifecycle::DecideHttpConnectUri(true, other, other, http, redirect), other);
}

// #414: the matchmaker queue connects to https://graph.oculus.com; with nothing configured it goes to nevr_http_uri.
TEST(DecideHttpConnectUri, TheMatchmakerQueuesGraphHostGoesToTheHttpServiceUnlessConfigured) {
  SetConfigInputs();
  int redirects = 0;
  const auto http = [] { return NevrCfgGetFlat("nevr_http_uri"); };
  const auto redirect = [&](const char* r, const char* t) { ++redirects; return NevrCfgRedirect(r, t, 0, 0); };
  const char graph[] = "https://graph.oculus.com";
  EXPECT_STREQ(nevr::lifecycle::DecideHttpConnectUri(true, graph, graph, http, redirect), "https://service.example:7350");
  EXPECT_EQ(redirects, 0) << "the readyatdawn.com policy is not involved";
  // A configured graph_host / graphservice_host wins and the fallback is not consulted.
  const char configured[] = "https://graph.configured.example";
  EXPECT_EQ(nevr::lifecycle::DecideHttpConnectUri(true, graph, configured, http, redirect), configured);
  // Not armed: the game's pointer.
  EXPECT_EQ(nevr::lifecycle::DecideHttpConnectUri(false, graph, graph, http, redirect), graph);
  // With a path the host is still the graph host; look-alikes and other Meta hosts are not.
  const char withPath[] = "https://graph.oculus.com/v1";
  EXPECT_STREQ(nevr::lifecycle::DecideHttpConnectUri(true, withPath, withPath, http, redirect),
               "https://service.example:7350");
  for (const char* other : {"https://graph.oculus.com.evil.example", "https://graph.oculus.comx", "http://graph.oculus.com",
                            "https://graph.facebook.com", "https://oculus.com"}) {
    EXPECT_EQ(nevr::lifecycle::DecideHttpConnectUri(true, other, other, http, redirect), other) << other;
  }
  // No http target configured: nothing to redirect to.
  SetEmptyInputs();
  EXPECT_EQ(nevr::lifecycle::DecideHttpConnectUri(true, graph, graph, http, redirect), graph);
}

TEST(DecideUnconfiguredApiRedirect, OtherHostsNotArmedAndNoTargetKeepTheGamesPointer) {
  SetConfigInputs();
  const auto http = [] { return NevrCfgGetFlat("nevr_http_uri"); };
  const auto redirect = [](const char* r, const char* t) { return NevrCfgRedirect(r, t, 0, 0); };
  const char login[] = "https://login.readyatdawn.com";
  const char graph[] = "https://graph.oculus.com";
  const char api[] = "https://api.readyatdawn.com";
  EXPECT_EQ(nevr::lifecycle::DecideUnconfiguredApiRedirect(true, login, http, redirect), login);
  EXPECT_EQ(nevr::lifecycle::DecideUnconfiguredApiRedirect(true, graph, http, redirect), graph);
  EXPECT_EQ(nevr::lifecycle::DecideUnconfiguredApiRedirect(false, api, http, redirect), api);
  EXPECT_EQ(nevr::lifecycle::DecideUnconfiguredApiRedirect(true, nullptr, http, redirect), nullptr);
  SetEmptyInputs();  // no http_uri configured: nothing to redirect to
  EXPECT_EQ(nevr::lifecycle::DecideUnconfiguredApiRedirect(true, api, http, redirect), api);
}

TEST(StableStringPoolAccessors, SelectedRedirectReplacesTheGameResultWithTheStablePointer) {
  SetConfigInputs();
  const char defaultValue[] = "wss://login.readyatdawn.com/rad15";
  const char* gameResult = defaultValue;
  const char* redirected = NevrCfgRedirect(gameResult, nullptr, 1, 53748);
  ASSERT_NE(redirected, nullptr);

  const char* chosen = nevr::lifecycle::ChooseRedirectedOrOriginal(gameResult, redirected);
  EXPECT_EQ(chosen, redirected);
  EXPECT_NE(chosen, gameResult);
  EXPECT_STREQ(chosen, "ws://127.0.0.1:53748");
  EXPECT_EQ(NevrCfgRedirect(gameResult, nullptr, 1, 53748), chosen);  // same stable pointer on a second lookup
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

// The injected-failure test above reports synthetic counts (strings=7
// live_bytes=13) because it fails ahead of InternStableCStr. This test drives
// the real pool to its count limit through an accessor and checks the counts the
// pool itself reports.
TEST(StableStringPoolAccessors, RealPoolCountLimitTerminatesThroughTheAccessorBoundary) {
  constexpr unsigned kLimit = static_cast<unsigned>(nevr::lifecycle::kStableStringMaxCount);
  std::size_t liveBytes = 0;
  for (unsigned port = 1; port <= kLimit; ++port) {
    liveBytes += std::string("ws://127.0.0.1:" + std::to_string(port)).size() + 1;  // payload plus NUL
  }
  const std::string expected =
      "^\\[NEVR\\.CONFIG\\] C accessor failed accessor=NevrCfgAutoRelay status=count_limit strings=" +
      std::to_string(kLimit) + " live_bytes=" + std::to_string(liveBytes) + "\\r?\\n$";
  ASSERT_EXIT(
      {
        SetConfigInputs();
        for (unsigned port = 1; port <= kLimit; ++port) {
          if (NevrCfgAutoRelay(port) == nullptr) ExitProcess(2);
        }
        (void)NevrCfgAutoRelay(kLimit + 1);
        ExitProcess(0);
      },
      ::testing::ExitedWithCode(1), expected);
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
