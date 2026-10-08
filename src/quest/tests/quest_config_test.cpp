// Host and Android test for the Quest configuration and activation logic (quest_config.cpp).
#include "quest/sentinel/quest_config.h"

#include <cstdio>
#include <string>

namespace {

int g_failures = 0;

#define CHECK(cond)                                                              \
  do {                                                                           \
    if (!(cond)) {                                                               \
      std::fprintf(stderr, "%s:%d CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
      ++g_failures;                                                              \
    }                                                                            \
  } while (0)

using nevr_quest::EmbeddedDefaults;
using nevr_quest::Feature;
using nevr_quest::LoadResult;
using nevr_quest::LogLevel;
using nevr_quest::Source;

// Synthetic values; the secrets below must never appear in a log event.
constexpr const char* kEmbSocket = "wss://emb.example/nevr";
constexpr const char* kEmbHttp = "https://emb.example:7350";
constexpr const char* kEmbApiKey = "EMBEDDED-API-KEY-8841";
constexpr const char* kEmbServerKey = "EMBEDDED-SERVER-KEY-7712";

EmbeddedDefaults Full() { return {kEmbSocket, kEmbHttp, kEmbApiKey, kEmbServerKey}; }

LoadResult Load(const EmbeddedDefaults& d, const std::string& file) { return nevr_quest::ResolveConfig(d, &file); }

bool EventsContain(const LoadResult& r, const std::string& needle) {
  for (const auto& e : r.events) {
    if (e.message.find(needle) != std::string::npos) return true;
  }
  return false;
}

bool HasLevel(const LoadResult& r, LogLevel level) {
  for (const auto& e : r.events) {
    if (e.level == level) return true;
  }
  return false;
}

bool AllOff(const nevr_quest::Features& f) { return !f.redirect && !f.bridge && !f.login; }

void DefaultsWithoutFile() {
  const LoadResult r = nevr_quest::ResolveConfig(Full(), nullptr);
  CHECK(!r.fileRejected);
  CHECK(r.config.socketUri.text == kEmbSocket && r.config.socketUri.source == Source::kEmbedded);
  CHECK(r.config.httpUri.text == kEmbHttp && r.config.httpUri.source == Source::kEmbedded);
  CHECK(r.config.httpKey.text == kEmbApiKey);
  CHECK(r.config.serverKey.text == kEmbServerKey);
  CHECK(AllOff(r.config.requested) && AllOff(r.config.effective));
  CHECK(EventsContain(r, "feature=login requested=off effective=off"));
  CHECK(EventsContain(r, "config key=nevr_socket_uri source=embedded"));
  CHECK(!HasLevel(r, LogLevel::kError));
}

void EmptyEmbeddedIsAbsent() {
  const LoadResult r = nevr_quest::ResolveConfig(EmbeddedDefaults(), nullptr);
  CHECK(r.config.socketUri.source == Source::kAbsent && r.config.socketUri.text.empty());
  CHECK(EventsContain(r, "config key=nevr_http_key source=absent"));
  CHECK(AllOff(r.config.effective));
}

void InvalidEmbeddedIsRejectedWithoutValue() {
  EmbeddedDefaults d = Full();
  d.socketUri = "http://SECRETHOST-9931/not-a-socket-scheme";
  const LoadResult r = nevr_quest::ResolveConfig(d, nullptr);
  CHECK(r.config.socketUri.source == Source::kAbsent);
  CHECK(EventsContain(r, "embedded default rejected key=nevr_socket_uri"));
  CHECK(!EventsContain(r, "SECRETHOST-9931"));
}

void FileOverridesPerKey() {
  const LoadResult r = Load(Full(), R"({"nevr_socket_uri":"ws://file.example/nevr","nevr_http_key":"FILE-KEY-1"})");
  CHECK(!r.fileRejected);
  CHECK(r.config.socketUri.text == "ws://file.example/nevr" && r.config.socketUri.source == Source::kFile);
  CHECK(r.config.httpKey.text == "FILE-KEY-1" && r.config.httpKey.source == Source::kFile);
  CHECK(r.config.httpUri.text == kEmbHttp && r.config.httpUri.source == Source::kEmbedded);
  CHECK(r.config.serverKey.text == kEmbServerKey);
  CHECK(AllOff(r.config.effective));
}

void FeaturesEnableWhenPrerequisitesHold() {
  const LoadResult r = Load(Full(), R"({"features":{"redirect":true,"bridge":true,"login":true}})");
  CHECK(r.config.requested.redirect && r.config.requested.bridge && r.config.requested.login);
  CHECK(r.config.effective.redirect && r.config.effective.bridge && r.config.effective.login);
  CHECK(nevr_quest::FeatureEnabled(r.config, Feature::kLogin));
  CHECK(EventsContain(r, "feature=login requested=on effective=on"));
  CHECK(!HasLevel(r, LogLevel::kWarn));
}

void FeatureDependenciesForceOff() {
  LoadResult r = Load(Full(), R"({"features":{"bridge":true,"login":true}})");
  CHECK(!r.config.effective.redirect && !r.config.effective.bridge && !r.config.effective.login);
  CHECK(r.config.requested.bridge && r.config.requested.login);
  CHECK(EventsContain(r, "feature=bridge forced off reason=redirect_not_enabled"));
  CHECK(EventsContain(r, "feature=login forced off reason=bridge_not_enabled"));

  r = Load(Full(), R"({"features":{"redirect":true,"login":true}})");
  CHECK(r.config.effective.redirect && !r.config.effective.bridge && !r.config.effective.login);

  EmbeddedDefaults noKey = Full();
  noKey.serverKey = "";
  r = Load(noKey, R"({"features":{"redirect":true,"bridge":true,"login":true}})");
  CHECK(r.config.effective.bridge && !r.config.effective.login);
  CHECK(EventsContain(r, "feature=login forced off reason=no_server_key"));

  r = Load(EmbeddedDefaults(), R"({"features":{"redirect":true,"bridge":true}})");
  CHECK(!r.config.effective.redirect && !r.config.effective.bridge);
  CHECK(EventsContain(r, "feature=redirect forced off reason=no_target_configured"));

  EmbeddedDefaults httpOnly;
  httpOnly.httpUri = kEmbHttp;
  r = Load(httpOnly, R"({"features":{"redirect":true,"bridge":true}})");
  CHECK(r.config.effective.redirect && !r.config.effective.bridge);
  CHECK(EventsContain(r, "feature=bridge forced off reason=no_socket_uri"));
}

void MalformedFileFallsBackToDefaultsWithFeaturesOff() {
  const char* bad[] = {"{", "", "not json", R"({"nevr_socket_uri":)", "[1,2,3]", "\"str\"", "42", "null"};
  for (const char* text : bad) {
    const LoadResult r = Load(Full(), text);
    CHECK(r.fileRejected);
    CHECK(r.config.socketUri.source == Source::kEmbedded);
    CHECK(r.config.serverKey.text == kEmbServerKey);
    CHECK(AllOff(r.config.requested) && AllOff(r.config.effective));
    CHECK(HasLevel(r, LogLevel::kError));
  }
}

void OversizedFileIsRejected() {
  std::string big = R"({"features":{"redirect":true},"pad":")";
  big.append(nevr_quest::kMaxConfigBytes, 'x');
  big += "\"}";
  const LoadResult r = Load(Full(), big);
  CHECK(r.fileRejected);
  CHECK(AllOff(r.config.effective));
  CHECK(EventsContain(r, "reason=too_large"));
}

void BadKeyValuesKeepTheDefault() {
  const LoadResult r = Load(Full(), R"({
    "nevr_socket_uri":"https://SECRETHOST-1/wrong-scheme",
    "nevr_http_uri":"ftp://SECRETHOST-2/",
    "nevr_http_key":"has a space SECRETKEY-3",
    "nevr_server_key":12345})");
  CHECK(!r.fileRejected);
  CHECK(r.config.socketUri.source == Source::kEmbedded);
  CHECK(r.config.httpUri.source == Source::kEmbedded);
  CHECK(r.config.httpKey.text == kEmbApiKey);
  CHECK(r.config.serverKey.text == kEmbServerKey);
  CHECK(EventsContain(r, "key=nevr_socket_uri rejected"));
  CHECK(EventsContain(r, "key=nevr_server_key rejected reason=not_a_string"));
  CHECK(!EventsContain(r, "SECRETHOST"));
  CHECK(!EventsContain(r, "SECRETKEY"));
}

void UriShapeValidation() {
  const char* bad[] = {"wss://", "wss:///path", "ws://?q=1", "wss://host name/x", "WSS://host/x", "ws:/host",
                       "host.example", ""};
  for (const char* uri : bad) {
    const std::string file = std::string(R"({"nevr_socket_uri":")") + uri + "\"}";
    const LoadResult r = Load(Full(), file);
    CHECK(r.config.socketUri.source == Source::kEmbedded);
  }
  const LoadResult ok = Load(Full(), R"({"nevr_socket_uri":"wss://h.example:443/nevr?format=evr"})");
  CHECK(ok.config.socketUri.source == Source::kFile);
}

void WrongFeatureTypesStayOff() {
  LoadResult r = Load(Full(), R"({"features":{"redirect":"yes","bridge":1,"login":null}})");
  CHECK(AllOff(r.config.requested));
  CHECK(EventsContain(r, "feature=redirect rejected reason=not_a_boolean"));

  r = Load(Full(), R"({"features":[true]})");
  CHECK(AllOff(r.config.requested));
  CHECK(EventsContain(r, "features rejected reason=not_an_object"));

  r = Load(Full(), R"({"features":{"redirect":true,"turbo":true}})");
  CHECK(r.config.effective.redirect);
  CHECK(EventsContain(r, "unknown feature=turbo"));
}

void UnknownKeysWarnWithoutLeakingValues() {
  const LoadResult r = Load(Full(), R"({"nevr_pasword":"SECRETPW-77","bad\u0001key":"x"})");
  CHECK(!r.fileRejected);
  CHECK(EventsContain(r, "unknown key=nevr_pasword ignored"));
  CHECK(EventsContain(r, "unknown key=<unloggable> ignored"));
  CHECK(!EventsContain(r, "SECRETPW-77"));
}

void NoEventEverCarriesAConfiguredValue() {
  const LoadResult r = Load(Full(), R"({"nevr_http_key":"FILE-SECRET-5","features":{"redirect":true,"bridge":true,"login":true}})");
  for (const char* secret : {kEmbSocket, kEmbHttp, kEmbApiKey, kEmbServerKey, "FILE-SECRET-5", "file.example"}) {
    CHECK(!EventsContain(r, secret));
  }
}

void ConfigPathIsNeverGameConfigJson() {
  const std::string a = nevr_quest::ConfigFilePath("/sdcard/x/files");
  const std::string b = nevr_quest::ConfigFilePath("/sdcard/x/files/");
  CHECK(a == "/sdcard/x/files/nevr-quest.json");
  CHECK(b == a);
  CHECK(a.find("config.json") == std::string::npos);
  CHECK(nevr_quest::ConfigFilePath("") == "nevr-quest.json");
}

void RedirectIsGatedByActivation() {
  const std::string wsUrl = "wss://login.readyatdawn.com/rad/rad15_live";
  const std::string httpUrl = "https://config.readyatdawn.com/rad/rad15_live";

  LoadResult off = Load(Full(), "{}");
  CHECK(!nevr_quest::ResolveQuestRedirect(off.config, wsUrl, true, 5000).has_value());

  LoadResult redirectOnly = Load(Full(), R"({"features":{"redirect":true}})");
  const auto ws = nevr_quest::ResolveQuestRedirect(redirectOnly.config, wsUrl, true, 5000);
  CHECK(ws.has_value() && *ws == kEmbSocket);
  const auto http = nevr_quest::ResolveQuestRedirect(redirectOnly.config, httpUrl, false, 0);
  CHECK(http.has_value() && *http == kEmbHttp);
  CHECK(!nevr_quest::ResolveQuestRedirect(redirectOnly.config, "https://other.example/x", false, 0).has_value());

  LoadResult bridged = Load(Full(), R"({"features":{"redirect":true,"bridge":true}})");
  const auto loop = nevr_quest::ResolveQuestRedirect(bridged.config, wsUrl, true, 5000);
  CHECK(loop.has_value() && *loop == "ws://127.0.0.1:5000");
  const auto notReady = nevr_quest::ResolveQuestRedirect(bridged.config, wsUrl, false, 0);
  CHECK(notReady.has_value() && *notReady == kEmbSocket);
  const auto httpBridged = nevr_quest::ResolveQuestRedirect(bridged.config, httpUrl, true, 5000);
  CHECK(httpBridged.has_value() && *httpBridged == kEmbHttp);
}

}  // namespace

int main() {
  DefaultsWithoutFile();
  EmptyEmbeddedIsAbsent();
  InvalidEmbeddedIsRejectedWithoutValue();
  FileOverridesPerKey();
  FeaturesEnableWhenPrerequisitesHold();
  FeatureDependenciesForceOff();
  MalformedFileFallsBackToDefaultsWithFeaturesOff();
  OversizedFileIsRejected();
  BadKeyValuesKeepTheDefault();
  UriShapeValidation();
  WrongFeatureTypesStayOff();
  UnknownKeysWarnWithoutLeakingValues();
  NoEventEverCarriesAConfiguredValue();
  ConfigPathIsNeverGameConfigJson();
  RedirectIsGatedByActivation();
  if (g_failures != 0) {
    std::fprintf(stderr, "quest_config_test: %d check(s) failed\n", g_failures);
    return 1;
  }
  std::printf("quest_config_test: all checks pass\n");
  return 0;
}
