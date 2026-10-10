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

bool AllOff(const nevr_quest::Features& f) {
  return !f.redirect && !f.bridge && !f.login && !f.social && !f.hwdump && !f.obbSkip && !f.presenceNames;
}

// Index of the first event whose message contains `needle`, or -1.
int EventIndex(const LoadResult& r, const std::string& needle) {
  for (std::size_t i = 0; i < r.events.size(); ++i) {
    if (r.events[i].message.find(needle) != std::string::npos) return static_cast<int>(i);
  }
  return -1;
}

void DefaultsWithoutFile() {
  const LoadResult r = nevr_quest::ResolveConfig(Full(), nullptr);
  CHECK(!r.fileRejected);
  CHECK(r.config.socketUri.text == kEmbSocket && r.config.socketUri.source == Source::kEmbedded);
  CHECK(r.config.httpUri.text == kEmbHttp && r.config.httpUri.source == Source::kEmbedded);
  CHECK(r.config.httpKey.text == kEmbApiKey);
  CHECK(r.config.serverKey.text == kEmbServerKey);
  CHECK(AllOff(r.config.requested) && AllOff(r.config.effective));
  CHECK(EventsContain(r, "feature=login requested=off effective=off"));
  CHECK(EventsContain(r, "feature=social requested=off effective=off"));
  CHECK(!nevr_quest::FeatureEnabled(r.config, Feature::kSocial));
  // The hardware dump (#335) is a diagnostic: off unless a file turns it on.
  CHECK(!r.config.requested.hwdump && !r.config.effective.hwdump);
  CHECK(!nevr_quest::FeatureEnabled(r.config, Feature::kHwDump));
  CHECK(EventsContain(r, "feature=hwdump requested=off effective=off"));
  // The OBB-mount skip (#319) is off until a file turns it on.
  CHECK(!r.config.requested.obbSkip && !r.config.effective.obbSkip);
  CHECK(!nevr_quest::FeatureEnabled(r.config, Feature::kObbSkip));
  CHECK(EventsContain(r, "feature=obb_skip requested=off effective=off"));
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
  const LoadResult r = Load(Full(), R"({"features":{"redirect":true,"bridge":true,"login":true,"social":true}})");
  CHECK(r.config.requested.redirect && r.config.requested.bridge && r.config.requested.login &&
        r.config.requested.social);
  CHECK(r.config.effective.redirect && r.config.effective.bridge && r.config.effective.login &&
        r.config.effective.social);
  CHECK(nevr_quest::FeatureEnabled(r.config, Feature::kSocial));
  CHECK(EventsContain(r, "feature=social requested=on effective=on"));
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

void SocialNeedsLoginAndResolvesLast() {
  // Social alone: login is not enabled, so social is forced off, with that one reason.
  LoadResult r = Load(Full(), R"({"features":{"social":true}})");
  CHECK(r.config.requested.social && !r.config.effective.social);
  CHECK(!nevr_quest::FeatureEnabled(r.config, Feature::kSocial));
  CHECK(EventsContain(r, "feature=social forced off reason=login_not_enabled"));
  CHECK(EventsContain(r, "feature=social requested=on effective=off"));

  // The chain: login loses its bridge, and social loses login, in that order in the log.
  r = Load(Full(), R"({"features":{"redirect":true,"login":true,"social":true}})");
  CHECK(r.config.effective.redirect && !r.config.effective.bridge && !r.config.effective.login &&
        !r.config.effective.social);
  const int loginOff = EventIndex(r, "feature=login forced off reason=bridge_not_enabled");
  const int socialOff = EventIndex(r, "feature=social forced off reason=login_not_enabled");
  CHECK(loginOff >= 0 && socialOff > loginOff);

  // Login that loses the server key takes social with it.
  EmbeddedDefaults noKey = Full();
  noKey.serverKey = "";
  r = Load(noKey, R"({"features":{"redirect":true,"bridge":true,"login":true,"social":true}})");
  CHECK(r.config.effective.bridge && !r.config.effective.login && !r.config.effective.social);
  CHECK(EventsContain(r, "feature=login forced off reason=no_server_key"));
  CHECK(EventsContain(r, "feature=social forced off reason=login_not_enabled"));

  // Login without social stays on.
  r = Load(Full(), R"({"features":{"redirect":true,"bridge":true,"login":true}})");
  CHECK(r.config.effective.login && !r.config.effective.social);
  CHECK(!EventsContain(r, "feature=social forced off"));

  // A malformed value stays off and is rejected by name, whatever login does.
  r = Load(Full(), R"({"features":{"redirect":true,"bridge":true,"login":true,"social":"yes"}})");
  CHECK(r.config.effective.login && !r.config.requested.social && !r.config.effective.social);
  CHECK(EventsContain(r, "feature=social rejected reason=not_a_boolean"));
  r = Load(Full(), R"({"features":{"redirect":true,"bridge":true,"login":true,"social":1}})");
  CHECK(!r.config.requested.social);

  // The name is the file key and the log name.
  CHECK(std::string(nevr_quest::FeatureName(Feature::kSocial)) == "social");
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
  CHECK(EventsContain(r, "unknown feature #1 ignored"));
}

void UnknownKeysWarnWithoutLeakingValues() {
  const LoadResult r = Load(Full(), R"({"nevr_pasword":"SECRETPW-77","bad\u0001key":"x"})");
  CHECK(!r.fileRejected);
  CHECK(EventsContain(r, "unknown key #1 ignored"));
  CHECK(EventsContain(r, "unknown key #2 ignored"));
  CHECK(!EventsContain(r, "SECRETPW-77"));

  // Names shaped like identifiers, hex digests or words are not echoed either.
  const LoadResult shaped = Load(Full(), R"({"5f4dcc3b5aa765d61d8327deb882cf99":"x","defaultkey":1,"defaultkey":2,)"
                                        R"("features":{"s3cr3tlowercase":true}})");
  CHECK(!shaped.fileRejected);
  for (const char* name : {"5f4dcc3b5aa765d61d8327deb882cf99", "defaultkey", "s3cr3tlowercase"}) {
    CHECK(!EventsContain(shaped, name));
  }
  CHECK(EventsContain(shaped, "unknown key #1 ignored") && EventsContain(shaped, "unknown feature #1 ignored"));
  CHECK(EventsContain(shaped, "duplicate key=<unknown> extra=1"));
}

void NoEventEverCarriesAConfiguredValue() {
  const LoadResult r = Load(Full(), R"({"nevr_http_key":"FILE-SECRET-5","features":{"redirect":true,"bridge":true,"login":true}})");
  for (const char* secret : {kEmbSocket, kEmbHttp, kEmbApiKey, kEmbServerKey, "FILE-SECRET-5", "file.example"}) {
    CHECK(!EventsContain(r, secret));
  }
}

void DuplicateKeysWarnAndTheLastValueWins() {
  const LoadResult r = Load(Full(), R"({"nevr_http_key":"FIRST-1","nevr_http_key":"LAST-2",)"
                                    R"("features":{"redirect":false,"redirect":true}})");
  CHECK(!r.fileRejected);
  CHECK(r.config.httpKey.text == "LAST-2");
  CHECK(r.config.effective.redirect);
  CHECK(EventsContain(r, "duplicate key=nevr_http_key extra=1 last value wins"));
  CHECK(EventsContain(r, "duplicate key=redirect extra=1 last value wins"));
  CHECK(!EventsContain(r, "FIRST-1") && !EventsContain(r, "LAST-2"));
  // The same key in two different objects is not a duplicate.
  const LoadResult ok = Load(Full(), R"({"features":{"redirect":true},"other":{"redirect":1}})");
  CHECK(!EventsContain(ok, "duplicate"));
}

void ManyBadKeysProduceBoundedLogging() {
  // 6000 copies of one key collapse to one warning.
  std::string dup = "{";
  for (int i = 0; i < 6000; ++i) dup += "\"a\":1,";
  dup += "\"nevr_http_uri\":\"https://h.example\"}";
  LoadResult r = Load(Full(), dup);
  CHECK(!r.fileRejected && r.config.httpUri.text == "https://h.example");
  CHECK(EventsContain(r, "duplicate key=<unknown> extra=5999 last value wins"));
  CHECK(r.events.size() < 40);

  // 5000 distinct unknown keys are capped, with one line saying how many were dropped.
  std::string many = "{";
  for (int i = 0; i < 5000; ++i) many += "\"u" + std::to_string(i) + "\":1,";
  many += "\"nevr_http_uri\":\"https://h.example\"}";
  r = Load(Full(), many);
  CHECK(!r.fileRejected);
  CHECK(r.events.size() < 60);
  CHECK(EventsContain(r, "further warnings suppressed count=4968"));
}

void AnEmptyFileValueCannotClearAnEmbeddedDefault() {
  const LoadResult r = Load(Full(), R"({"nevr_http_key":"","nevr_socket_uri":""})");
  CHECK(r.config.httpKey.text == kEmbApiKey && r.config.httpKey.source == Source::kEmbedded);
  CHECK(r.config.socketUri.text == kEmbSocket);
  CHECK(EventsContain(r, "key=nevr_http_key rejected reason=empty"));
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

// The hardware dump (#335) needs nothing else: the file's boolean alone turns it on, and anything but a
// JSON true leaves it off.
void HwDumpIsOnlyEverOnByAFileBoolean() {
  {
    const LoadResult r = Load(Full(), R"({"features":{"hwdump":true}})");
    CHECK(r.config.requested.hwdump && r.config.effective.hwdump);
    CHECK(nevr_quest::FeatureEnabled(r.config, Feature::kHwDump));
    CHECK(!r.config.effective.redirect && !r.config.effective.bridge && !r.config.effective.login &&
          !r.config.effective.social);
    CHECK(EventsContain(r, "feature=hwdump requested=on effective=on"));
    CHECK(!HasLevel(r, LogLevel::kWarn));
  }
  for (const char* text : {R"({"features":{"hwdump":"true"}})", R"({"features":{"hwdump":1}})",
                           R"({"hwdump":true})", R"({"features":{}})"}) {
    const LoadResult r = Load(Full(), text);
    CHECK(!r.config.effective.hwdump);
    CHECK(EventsContain(r, "feature=hwdump requested=off effective=off"));
  }
}

// The OBB-mount skip (#319) needs nothing else: the file's boolean alone turns it on, and anything but a
// JSON true leaves it off.
void ObbSkipIsOnlyEverOnByAFileBoolean() {
  {
    const LoadResult r = Load(Full(), R"({"features":{"obb_skip":true}})");
    CHECK(r.config.requested.obbSkip && r.config.effective.obbSkip);
    CHECK(nevr_quest::FeatureEnabled(r.config, Feature::kObbSkip));
    CHECK(!r.config.effective.redirect && !r.config.effective.bridge && !r.config.effective.login &&
          !r.config.effective.social && !r.config.effective.hwdump);
    CHECK(EventsContain(r, "feature=obb_skip requested=on effective=on"));
    CHECK(!HasLevel(r, LogLevel::kWarn));
  }
  for (const char* text : {R"({"features":{"obb_skip":"true"}})", R"({"features":{"obb_skip":1}})",
                           R"({"obb_skip":true})", R"({"features":{}})"}) {
    const LoadResult r = Load(Full(), text);
    CHECK(!r.config.effective.obbSkip);
    CHECK(EventsContain(r, "feature=obb_skip requested=off effective=off"));
  }
  // A rejected file contributes nothing, so the feature stays off.
  const LoadResult rejected = Load(Full(), R"({"features":{"obb_skip":true})");
  CHECK(rejected.fileRejected && !rejected.config.effective.obbSkip);
}

// Presence names (#393) ride in the social facade's member data: off unless the file asks, forced off without
// social (so without login), and only a JSON true counts.
void PresenceNamesNeedSocialAndAreOffByDefault() {
  const char* all = R"({"features":{"redirect":true,"bridge":true,"login":true,"social":true,"presence_names":true}})";
  LoadResult r = Load(Full(), all);
  CHECK(r.config.requested.presenceNames && r.config.effective.presenceNames);
  CHECK(nevr_quest::FeatureEnabled(r.config, Feature::kPresenceNames));
  CHECK(EventsContain(r, "feature=presence_names requested=on effective=on"));
  CHECK(!HasLevel(r, LogLevel::kWarn));

  // Without social it is forced off, with that one reason, and after social's own rules ran.
  r = Load(Full(), R"({"features":{"presence_names":true}})");
  CHECK(r.config.requested.presenceNames && !r.config.effective.presenceNames);
  CHECK(EventsContain(r, "feature=presence_names forced off reason=social_not_enabled"));
  r = Load(Full(), R"({"features":{"redirect":true,"bridge":true,"login":true,"presence_names":true}})");
  CHECK(r.config.effective.login && !r.config.effective.social && !r.config.effective.presenceNames);
  r = Load(Full(), R"({"features":{"social":true,"presence_names":true}})");  // social loses login, so it loses this too
  CHECK(!r.config.effective.social && !r.config.effective.presenceNames);
  CHECK(EventsContain(r, "feature=presence_names forced off reason=social_not_enabled"));

  // Off by default, and a non-boolean leaves it off.
  r = Load(Full(), R"({"features":{"redirect":true,"bridge":true,"login":true,"social":true}})");
  CHECK(r.config.effective.social && !r.config.requested.presenceNames && !r.config.effective.presenceNames);
  CHECK(EventsContain(r, "feature=presence_names requested=off effective=off"));
  for (const char* text : {R"({"features":{"presence_names":"true"}})", R"({"features":{"presence_names":1}})",
                           R"({"presence_names":true})"}) {
    r = Load(Full(), text);
    CHECK(!r.config.effective.presenceNames);
  }
}

int main() {
  DefaultsWithoutFile();
  EmptyEmbeddedIsAbsent();
  InvalidEmbeddedIsRejectedWithoutValue();
  FileOverridesPerKey();
  FeaturesEnableWhenPrerequisitesHold();
  FeatureDependenciesForceOff();
  SocialNeedsLoginAndResolvesLast();
  MalformedFileFallsBackToDefaultsWithFeaturesOff();
  OversizedFileIsRejected();
  BadKeyValuesKeepTheDefault();
  UriShapeValidation();
  WrongFeatureTypesStayOff();
  UnknownKeysWarnWithoutLeakingValues();
  NoEventEverCarriesAConfiguredValue();
  DuplicateKeysWarnAndTheLastValueWins();
  ManyBadKeysProduceBoundedLogging();
  AnEmptyFileValueCannotClearAnEmbeddedDefault();
  ConfigPathIsNeverGameConfigJson();
  RedirectIsGatedByActivation();
  HwDumpIsOnlyEverOnByAFileBoolean();
  ObbSkipIsOnlyEverOnByAFileBoolean();
  PresenceNamesNeedSocialAndAreOffByDefault();
  if (g_failures != 0) {
    std::fprintf(stderr, "quest_config_test: %d check(s) failed\n", g_failures);
    return 1;
  }
  std::printf("quest_config_test: all checks pass\n");
  return 0;
}
