// Host and Android test for the Quest configuration and activation logic (quest_config.cpp). The file is
// config.yaml, parsed by the PC's nevr_config (src/core/nevr_config.cpp).
#include "quest/sentinel/quest_config.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
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

// True when at least one event contains `needle` and every one that does has `level`.
bool EventsWithAreAtLevel(const LoadResult& r, const std::string& needle, LogLevel level) {
  bool any = false;
  for (const auto& e : r.events) {
    if (e.message.find(needle) == std::string::npos) continue;
    if (e.level != level) return false;
    any = true;
  }
  return any;
}

bool HasLevel(const LoadResult& r, LogLevel level) {
  for (const auto& e : r.events) {
    if (e.level == level) return true;
  }
  return false;
}

std::size_t CountLevel(const LoadResult& r, LogLevel level) {
  std::size_t n = 0;
  for (const auto& e : r.events) n += e.level == level ? 1 : 0;
  return n;
}

// Everything the file does not switch: redirect, bridge, login, social and the names on, the diagnostics and the
// local presence off.
bool DefaultsOnly(const nevr_quest::Features& f) {
  return f.redirect && f.bridge && f.login && f.social && f.presenceNames && !f.hwdump && !f.presenceLocal;
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
  // No key switches redirect, bridge or login: the embedded service endpoints and key turn them on, and the
  // social facade is on by default, with the status names that ride in it.
  CHECK(DefaultsOnly(r.config.requested) && DefaultsOnly(r.config.effective));
  CHECK(EventsContain(r, "feature=redirect requested=on effective=on"));
  CHECK(EventsContain(r, "feature=bridge requested=on effective=on"));
  CHECK(EventsContain(r, "feature=login requested=on effective=on"));
  CHECK(EventsContain(r, "feature=social requested=on effective=on"));
  CHECK(EventsContain(r, "feature=presence_names requested=on effective=on"));
  CHECK(EventsContain(r, "feature=presence_local requested=off effective=off"));
  CHECK(nevr_quest::FeatureEnabled(r.config, Feature::kSocial));
  // The hardware dump (#335) is a diagnostic: off unless the file turns it on.
  CHECK(!r.config.requested.hwdump && !r.config.effective.hwdump);
  CHECK(!nevr_quest::FeatureEnabled(r.config, Feature::kHwDump));
  CHECK(EventsContain(r, "feature=hwdump requested=off effective=off"));
  // The OBB-mount skip (#319) is not a feature (#572): the feature report does not name it.
  CHECK(!EventsContain(r, "obb_skip"));
  // The keys are named by their config.yaml path, and the file's absence is logged with the file name.
  CHECK(EventsContain(r, "config key=services.socket_uri source=embedded"));
  CHECK(EventsContain(r, "config key=auth.http_uri source=embedded"));
  CHECK(EventsContain(r, "config key=auth.http_key source=embedded"));
  CHECK(EventsContain(r, "config key=auth.server_key source=embedded"));
  CHECK(EventsContain(r, "config file=config.yaml absent, using embedded defaults"));
  CHECK(!HasLevel(r, LogLevel::kError) && !HasLevel(r, LogLevel::kWarn));
}

void NothingEmbeddedAndNoFileTurnsTheNetworkFeaturesOff() {
  const LoadResult r = nevr_quest::ResolveConfig(EmbeddedDefaults(), nullptr);
  CHECK(r.config.socketUri.source == Source::kAbsent && r.config.socketUri.text.empty());
  CHECK(EventsContain(r, "config key=auth.http_key source=absent"));
  CHECK(!r.config.effective.redirect && !r.config.effective.bridge && !r.config.effective.login &&
        !r.config.effective.social && !r.config.effective.presenceNames);
  CHECK(EventsContain(r, "feature=redirect forced off reason=no_target_configured"));
  CHECK(EventsContain(r, "feature=bridge forced off reason=redirect_not_enabled"));
  CHECK(EventsContain(r, "feature=login forced off reason=bridge_not_enabled"));
  CHECK(EventsContain(r, "feature=social forced off reason=login_not_enabled"));
}

void InvalidEmbeddedIsRejectedWithoutValue() {
  EmbeddedDefaults d = Full();
  d.socketUri = "http://SECRETHOST-9931/not-a-socket-scheme";
  const LoadResult r = nevr_quest::ResolveConfig(d, nullptr);
  CHECK(r.config.socketUri.source == Source::kAbsent);
  CHECK(EventsContain(r, "embedded default rejected key=services.socket_uri"));
  CHECK(!EventsContain(r, "SECRETHOST-9931"));
}

void FileOverridesPerKey() {
  const LoadResult r = Load(Full(),
                            "services:\n  socket_uri: \"ws://file.example/nevr\"\n"
                            "auth:\n  http_key: \"FILE-KEY-1\"\n");
  CHECK(!r.fileRejected);
  CHECK(r.config.socketUri.text == "ws://file.example/nevr" && r.config.socketUri.source == Source::kFile);
  CHECK(r.config.httpKey.text == "FILE-KEY-1" && r.config.httpKey.source == Source::kFile);
  CHECK(r.config.httpUri.text == kEmbHttp && r.config.httpUri.source == Source::kEmbedded);
  CHECK(r.config.serverKey.text == kEmbServerKey);
  CHECK(DefaultsOnly(r.config.effective));
  CHECK(EventsContain(r, "config key=services.socket_uri source=file"));
  CHECK(EventsContain(r, "config file=config.yaml loaded"));
  CHECK(!EventsContain(r, "absent"));
}

void AnEmptyDocumentIsAFileThatChangesNothing() {
  for (const char* text : {"", "# only a comment\n", "---\n", "version: \"1\"\n", "x-notes:\n  anything: goes\n"}) {
    const LoadResult r = Load(Full(), text);
    CHECK(!r.fileRejected);
    CHECK(r.config.socketUri.source == Source::kEmbedded);
    CHECK(DefaultsOnly(r.config.effective));
    CHECK(EventsContain(r, "config file=config.yaml loaded"));
  }
}

void ASocketUriAloneTurnsRedirectAndBridgeOnAndLoginNeedsTheServerKey() {
  // No embedded value at all: the file's services.socket_uri is the only target.
  LoadResult r = Load(EmbeddedDefaults(), "services:\n  socket_uri: \"wss://file.example/nevr\"\n");
  CHECK(r.config.effective.redirect && r.config.effective.bridge);
  CHECK(!r.config.effective.login && !r.config.effective.social && !r.config.effective.presenceNames);
  CHECK(EventsContain(r, "feature=login forced off reason=no_server_key"));
  CHECK(EventsContain(r, "feature=social forced off reason=login_not_enabled"));
  CHECK(EventsContain(r, "feature=presence_names forced off reason=social_not_enabled"));

  // With auth.server_key in the file as well, login and social follow.
  r = Load(EmbeddedDefaults(),
           "services:\n  socket_uri: \"wss://file.example/nevr\"\nauth:\n  server_key: \"FILE-SERVER-KEY-3\"\n");
  CHECK(DefaultsOnly(r.config.effective));
  CHECK(!HasLevel(r, LogLevel::kWarn));

  // The embedded server key with the file's socket URI is enough too.
  EmbeddedDefaults keyOnly;
  keyOnly.serverKey = kEmbServerKey;
  r = Load(keyOnly, "services:\n  socket_uri: \"wss://file.example/nevr\"\n");
  CHECK(DefaultsOnly(r.config.effective));
}

void FeatureDependenciesForceOff() {
  // An HTTP target alone: redirect is on, the bridge needs a socket URI.
  EmbeddedDefaults httpOnly;
  httpOnly.httpUri = kEmbHttp;
  LoadResult r = nevr_quest::ResolveConfig(httpOnly, nullptr);
  CHECK(r.config.effective.redirect && !r.config.effective.bridge && !r.config.effective.login);
  CHECK(EventsContain(r, "feature=bridge forced off reason=no_socket_uri"));
  CHECK(EventsContain(r, "feature=login forced off reason=bridge_not_enabled"));

  // No server key: the bridge stays, login and what rides on it do not.
  EmbeddedDefaults noKey = Full();
  noKey.serverKey = "";
  r = nevr_quest::ResolveConfig(noKey, nullptr);
  CHECK(r.config.effective.redirect && r.config.effective.bridge && !r.config.effective.login);
  CHECK(EventsContain(r, "feature=login forced off reason=no_server_key"));
  CHECK(!r.config.effective.social && !r.config.effective.presenceNames);
}

void SocialNeedsLoginAndResolvesLast() {
  // The chain: login loses its key, and social and the names lose login, in that order in the log.
  EmbeddedDefaults noKey = Full();
  noKey.serverKey = "";
  const LoadResult r = nevr_quest::ResolveConfig(noKey, nullptr);
  const int loginOff = EventIndex(r, "feature=login forced off reason=no_server_key");
  const int socialOff = EventIndex(r, "feature=social forced off reason=login_not_enabled");
  const int namesOff = EventIndex(r, "feature=presence_names forced off reason=social_not_enabled");
  CHECK(loginOff >= 0 && socialOff > loginOff && namesOff > socialOff);
  // The name is the file key and the log name.
  CHECK(std::string(nevr_quest::FeatureName(Feature::kSocial)) == "social");
}

void SocialFacadeFalseTurnsSocialAndItsNamesOffAndLeavesThePresenceObjectAlone() {
  LoadResult r = Load(Full(), "social:\n  facade: false\n");
  CHECK(!r.fileRejected);
  CHECK(r.config.effective.login && !r.config.requested.social && !r.config.effective.social);
  CHECK(!r.config.effective.presenceNames && !r.config.effective.presenceLocal);
  CHECK(EventsContain(r, "feature=social requested=off effective=off"));
  CHECK(EventsContain(r, "feature=presence_names requested=off effective=off"));
  CHECK(!nevr_quest::FeatureEnabled(r.config, Feature::kSocial));
  CHECK(!nevr_quest::FeatureEnabled(r.config, Feature::kPresenceNames));
  CHECK(!HasLevel(r, LogLevel::kWarn));

  // The local presence answers ride in the facade too: asked for with the facade off, it is forced off by name.
  r = Load(Full(), "social:\n  facade: false\n  send_presence_to_meta: false\n");
  CHECK(r.config.requested.presenceLocal && !r.config.effective.presenceLocal);
  CHECK(EventsContain(r, "feature=presence_local forced off reason=social_not_enabled"));

  // YAML's own boolean spellings count, like on the PC.
  for (const char* spelling : {"no", "off", "False", "0"}) {
    r = Load(Full(), std::string("social:\n  facade: ") + spelling + "\n");
    CHECK(!r.config.effective.social);
  }
  for (const char* spelling : {"true", "yes", "on", "1"}) {
    r = Load(Full(), std::string("social:\n  facade: ") + spelling + "\n");
    CHECK(r.config.effective.social);
  }
}

void ABooleanThatIsNotOneKeepsItsDefaultAndIsRejectedByName() {
  LoadResult r = Load(Full(), "social:\n  facade: maybe\n");
  CHECK(!r.fileRejected && r.config.effective.social);
  CHECK(EventsContain(r, "config file key=social.facade rejected reason=not_a_boolean"));
  CHECK(EventsWithAreAtLevel(r, "config file key=social.facade rejected reason=not_a_boolean", LogLevel::kWarn));
  CHECK(!EventsContain(r, "maybe"));
  r = Load(Full(), "diagnostics:\n  hardware_dump: sure\n");
  CHECK(!r.config.effective.hwdump);
  CHECK(EventsContain(r, "config file key=diagnostics.hardware_dump rejected reason=not_a_boolean"));
  r = Load(Full(), "social:\n  send_presence_to_meta: [1, 2]\n");
  CHECK(!r.config.requested.presenceLocal);
  CHECK(!r.fileRejected);
}

void SendPresenceToMetaFalseIsTheLocalPresencePath() {
  LoadResult r = Load(Full(), "social:\n  send_presence_to_meta: false\n");
  CHECK(r.config.requested.presenceLocal && r.config.effective.presenceLocal);
  CHECK(nevr_quest::FeatureEnabled(r.config, Feature::kPresenceLocal));
  CHECK(EventsContain(r, "feature=presence_local requested=on effective=on"));
  CHECK(r.config.effective.presenceNames);  // independent of the local-presence path
  CHECK(!HasLevel(r, LogLevel::kWarn));

  // true is the default: the presence goes to Meta's platform service as before.
  r = Load(Full(), "social:\n  send_presence_to_meta: true\n");
  CHECK(!r.config.requested.presenceLocal && !r.config.effective.presenceLocal);
  CHECK(EventsContain(r, "feature=presence_local requested=off effective=off"));
}

void HardwareDumpIsOnlyEverOnByTheDiagnosticsKey() {
  {
    const LoadResult r = Load(Full(), "diagnostics:\n  hardware_dump: true\n");
    CHECK(r.config.requested.hwdump && r.config.effective.hwdump);
    CHECK(nevr_quest::FeatureEnabled(r.config, Feature::kHwDump));
    CHECK(r.config.effective.redirect && r.config.effective.bridge && r.config.effective.login &&
          r.config.effective.social);
    CHECK(EventsContain(r, "feature=hwdump requested=on effective=on"));
    CHECK(!HasLevel(r, LogLevel::kWarn));
  }
  // It needs nothing else: with no service target at all it is still on.
  {
    const LoadResult r = Load(EmbeddedDefaults(), "diagnostics:\n  hardware_dump: true\n");
    CHECK(r.config.effective.hwdump && !r.config.effective.redirect);
  }
  // The spellings that turn it on (nevr_config's ParseBool, then yaml-cpp's boolean spellings), and some that do not.
  for (const char* word : {"true", "True", "TRUE", "yes", "Yes", "YES", "on", "On", "ON", "1", "y", "Y", "tRuE"}) {
    const LoadResult r = Load(Full(), std::string("diagnostics:\n  hardware_dump: ") + word + "\n");
    CHECK(r.config.effective.hwdump);
  }
  for (const char* word : {"false", "no", "n", "off", "0", "2", "t", "enable", "maybe", "\"${NEVR_QUEST_TEST_UNSET_HW_ZXQ}\""}) {
    ::unsetenv("NEVR_QUEST_TEST_UNSET_HW_ZXQ");
    const LoadResult r = Load(Full(), std::string("diagnostics:\n  hardware_dump: ") + word + "\n");
    CHECK(!r.config.effective.hwdump);
  }
  // Anywhere but diagnostics.hardware_dump leaves it off; so does false and the old feature name.
  for (const char* text : {"diagnostics:\n  hardware_dump: false\n", "diagnostics:\n  hwdump: true\n",
                           "social:\n  hardware_dump: true\n", "hardware_dump: true\n", "x-hardware_dump: true\n",
                           "diagnostics: {}\n"}) {
    const LoadResult r = Load(Full(), text);
    CHECK(!r.config.effective.hwdump);
    CHECK(EventsContain(r, "feature=hwdump requested=off effective=off"));
  }
}

void MalformedFileFallsBackToDefaults() {
  struct Case {
    const char* text;
    const char* reason;
  };
  const Case bad[] = {
      {"services: [unterminated\n", "yaml_syntax"},
      {"auth: {http_key: x\n", "yaml_syntax"},
      {"- a\n- b\n", "not_a_mapping"},
      {"\"str\"\n", "not_a_mapping"},
      {"42\n", "not_a_mapping"},
      {"bogus_section:\n  x: 1\n", "unknown_top_level_key"},
      {"services:\n  socket_uri: \"ws://file.example/x\"\nbogus: 1\n", "unknown_top_level_key"},
      {"plugins: 3\n", "bad_plugins"},
  };
  for (const Case& c : bad) {
    const LoadResult r = Load(Full(), c.text);
    CHECK(r.fileRejected);
    // A rejected file contributes nothing: the embedded defaults stay.
    CHECK(r.config.socketUri.source == Source::kEmbedded && r.config.socketUri.text == kEmbSocket);
    CHECK(r.config.serverKey.text == kEmbServerKey);
    CHECK(DefaultsOnly(r.config.effective));
    // One Error line, which names the file and the reason, and says what stays.
    CHECK(CountLevel(r, LogLevel::kError) == 1);
    const std::string line = std::string("config file=config.yaml rejected reason=") + c.reason + " (embedded defaults stay)";
    CHECK(EventsContain(r, line));
    CHECK(!EventsContain(r, "loaded"));
  }
}

void ARejectedFileNeverEchoesWhatItHeld() {
  // A value pasted as a top-level key, a secret in a line the parser chokes on: none of it is in any event.
  for (const char* text : {"5f4dcc3b5aa765d61d8327deb882cf99: x\n", "SECRETPW-77: x\n",
                           "auth:\n  http_key: SECRETPW-77\n  bad: [\n", "SECRETPW-77\n- x\n"}) {
    const LoadResult r = Load(Full(), text);
    CHECK(r.fileRejected);
    CHECK(!EventsContain(r, "SECRETPW-77"));
    CHECK(!EventsContain(r, "5f4dcc3b5aa765d61d8327deb882cf99"));
  }
}

void AnUnknownKeyInsideAKnownSectionIsNotAnError() {
  const LoadResult r = Load(Full(), "auth:\n  http_kee: \"TYPO-VALUE-1\"\nservices:\n  somthing: 1\n");
  CHECK(!r.fileRejected && !HasLevel(r, LogLevel::kError));
  CHECK(r.config.httpKey.source == Source::kEmbedded);
  CHECK(!EventsContain(r, "TYPO-VALUE-1"));
}

void OversizedFileIsRejected() {
  std::string big = "services:\n  socket_uri: \"ws://file.example/x\"\nx-pad: \"";
  big.append(nevr_quest::kMaxConfigBytes, 'x');
  big += "\"\n";
  const LoadResult r = Load(Full(), big);
  CHECK(r.fileRejected);
  CHECK(DefaultsOnly(r.config.effective) && r.config.socketUri.source == Source::kEmbedded);
  CHECK(EventsContain(r, "config file=config.yaml rejected reason=too_large limit_bytes=65536 (embedded defaults stay)"));
}

// A file of exactly kMaxConfigBytes is read; one byte more is rejected (ApplyFile: `>`, not `>=`).
void TheSizeCapIsInclusive() {
  const std::string head = "services:\n  socket_uri: \"ws://file.example/x\"\nx-pad: \"";
  const std::string tail = "\"\n";
  std::string atCap = head;
  atCap.append(nevr_quest::kMaxConfigBytes - head.size() - tail.size(), 'x');
  atCap += tail;
  CHECK(atCap.size() == nevr_quest::kMaxConfigBytes);
  LoadResult r = Load(Full(), atCap);
  CHECK(!r.fileRejected);
  CHECK(r.config.socketUri.source == Source::kFile && r.config.socketUri.text == "ws://file.example/x");
  CHECK(EventsContain(r, "config file=config.yaml loaded"));
  std::string overCap = atCap;
  overCap.insert(head.size(), "x");
  CHECK(overCap.size() == nevr_quest::kMaxConfigBytes + 1);
  r = Load(Full(), overCap);
  CHECK(r.fileRejected && r.config.socketUri.source == Source::kEmbedded);
  CHECK(EventsContain(r, "config file=config.yaml rejected reason=too_large limit_bytes=65536 (embedded defaults stay)"));
}

// An alias can make the tree a cycle or expand it exponentially. The walks over it are bounded, so the file is
// rejected by name; unbounded, the cycles end the process and the nested aliases run for seconds.
std::string NestedAliases(int levels) {
  std::string s = "x-a0: &a0 [\"lol\"]\n";
  for (int i = 1; i <= levels; ++i) {
    s += "x-a" + std::to_string(i) + ": &a" + std::to_string(i) + " [";
    for (int j = 0; j < 10; ++j) s += (j ? ",*a" : "*a") + std::to_string(i - 1);
    s += "]\n";
  }
  return s;
}

// A rejection takes at most kMaxConfigNodes visits; the unbounded walk over 8 nested levels ran for 10.8 s.
constexpr std::chrono::milliseconds kRejectionBudget{100};

LoadResult TimedLoad(const std::string& file, bool& withinBudget) {
  const auto start = std::chrono::steady_clock::now();
  LoadResult r = Load(Full(), file);
  withinBudget = std::chrono::steady_clock::now() - start < kRejectionBudget;
  return r;
}

void AliasesThatLoopOrExplodeAreRejectedNotWalkedForever() {
  const char* const nesting =
      "config file=config.yaml rejected reason=nesting_too_deep (embedded defaults stay)";
  bool fast = false;
  LoadResult r = TimedLoad("x-a: &a [*a]\n", fast);
  CHECK(r.fileRejected && EventsContain(r, nesting) && fast);
  CHECK(DefaultsOnly(r.config.effective) && r.config.socketUri.source == Source::kEmbedded);
  r = TimedLoad("x-a: &a {k: *a}\n", fast);
  CHECK(r.fileRejected && EventsContain(r, nesting) && fast);
  r = TimedLoad("plugins:\n  - name: p\n    args: &a {k: *a}\n", fast);
  CHECK(r.fileRejected && EventsContain(r, nesting) && fast);
  r = TimedLoad(NestedAliases(8), fast);
  CHECK(r.fileRejected && fast);
  CHECK(EventsContain(r, "config file=config.yaml rejected reason=too_many_nodes (embedded defaults stay)"));
  CHECK(!EventsContain(r, "lol"));
}

void ADeepButLegalNestingStillLoads() {
  const std::string deep = "services:\n  socket_uri: \"ws://file.example/x\"\nx-d: " + std::string(32, '[') + "s" +
                           std::string(32, ']') + "\n";
  const LoadResult r = Load(Full(), deep);
  CHECK(!r.fileRejected && r.config.socketUri.source == Source::kFile);
  CHECK(EventsContain(r, "config file=config.yaml loaded"));
  // Far deeper than any cap: the parser's own limit or ours rejects it, never a crash.
  const std::string tooDeep = std::string(30000, '[') + std::string(30000, ']') + "\n";
  CHECK(Load(Full(), tooDeep).fileRejected);
}

void BadKeyValuesKeepTheDefault() {
  const LoadResult r = Load(Full(),
                            "services:\n  socket_uri: \"https://SECRETHOST-1/wrong-scheme\"\n"
                            "auth:\n  http_uri: \"ftp://SECRETHOST-2/\"\n"
                            "  http_key: \"has a space SECRETKEY-3\"\n"
                            "  server_key: \"\"\n");
  CHECK(!r.fileRejected);
  CHECK(r.config.socketUri.source == Source::kEmbedded);
  CHECK(r.config.httpUri.source == Source::kEmbedded);
  CHECK(r.config.httpKey.text == kEmbApiKey);
  CHECK(r.config.serverKey.text == kEmbServerKey);
  CHECK(EventsContain(r, "config file key=services.socket_uri rejected reason=scheme must be ws:// or wss://"));
  CHECK(EventsContain(r, "config file key=auth.http_uri rejected"));
  CHECK(EventsContain(r, "config file key=auth.http_key rejected reason=contains whitespace or a control character"));
  CHECK(EventsContain(r, "config file key=auth.server_key rejected reason=empty"));
  CHECK(EventsWithAreAtLevel(r, "config file key=auth.server_key rejected reason=empty", LogLevel::kWarn));
  CHECK(EventsWithAreAtLevel(r, "config file key=services.socket_uri rejected reason=", LogLevel::kWarn));
  CHECK(!EventsContain(r, "SECRETHOST"));
  CHECK(!EventsContain(r, "SECRETKEY"));
}

void UriShapeValidation() {
  const char* bad[] = {"wss://", "wss:///path", "ws://?q=1", "wss://host name/x", "WSS://host/x", "ws:/host",
                       "host.example", ""};
  for (const char* uri : bad) {
    const std::string file = std::string("services:\n  socket_uri: \"") + uri + "\"\n";
    const LoadResult r = Load(Full(), file);
    CHECK(r.config.socketUri.source == Source::kEmbedded);
  }
  const LoadResult ok = Load(Full(), "services:\n  socket_uri: \"wss://h.example:443/nevr?format=evr\"\n");
  CHECK(ok.config.socketUri.source == Source::kFile);
}

void AScalarOfAnyYamlTypeIsAValueLikeOnThePc() {
  // PC reads every scalar as its text; so does the Quest. A number is a (valid) key.
  const LoadResult r = Load(Full(), "auth:\n  server_key: 12345\n");
  CHECK(r.config.serverKey.text == "12345" && r.config.serverKey.source == Source::kFile);
}

void AnUnsetVariableInAKeyIsRejectedByKeyNotByName() {
  ::unsetenv("NEVR_QUEST_TEST_UNSET_VAR_ZXQ");
  ::setenv("NEVR_QUEST_TEST_SET_VAR_ZXQ", "FROM-THE-ENVIRONMENT-5", 1);
  LoadResult r = Load(Full(),
                      "auth:\n  http_key: \"${NEVR_QUEST_TEST_UNSET_VAR_ZXQ}\"\n"
                      "  server_key: \"${NEVR_QUEST_TEST_SET_VAR_ZXQ}\"\n");
  CHECK(!r.fileRejected);
  // The unset reference does not become a credential: the key counts as unset and the default stays.
  CHECK(r.config.httpKey.source == Source::kEmbedded && r.config.httpKey.text == kEmbApiKey);
  CHECK(EventsContain(r, "config file key=auth.http_key rejected reason=unset_variable"));
  CHECK(EventsWithAreAtLevel(r, "config file key=auth.http_key rejected reason=unset_variable", LogLevel::kWarn));
  CHECK(EventsContain(r, "unset_variable_references=1"));
  CHECK(EventsWithAreAtLevel(r, "unset_variable_references=1", LogLevel::kWarn));
  CHECK(!EventsContain(r, "NEVR_QUEST_TEST_UNSET_VAR_ZXQ"));
  // A boolean key that holds an unset reference keeps its default and is rejected by key.
  r = Load(Full(), "social:\n  facade: \"${NEVR_QUEST_TEST_UNSET_VAR_ZXQ}\"\n"
                   "diagnostics:\n  hardware_dump: \"${NEVR_QUEST_TEST_UNSET_VAR_ZXQ}\"\n");
  CHECK(r.config.effective.social && !r.config.effective.hwdump);
  CHECK(EventsContain(r, "config file key=social.facade rejected reason=unset_variable (default stays)"));
  CHECK(EventsWithAreAtLevel(r, "key=social.facade rejected reason=unset_variable (default stays)", LogLevel::kWarn));
  CHECK(EventsContain(r, "config file key=diagnostics.hardware_dump rejected reason=unset_variable (default stays)"));
  CHECK(EventsContain(r, "unset_variable_references=1"));
  r = Load(Full(),
           "auth:\n  http_key: \"${NEVR_QUEST_TEST_UNSET_VAR_ZXQ}\"\n"
           "  server_key: \"${NEVR_QUEST_TEST_SET_VAR_ZXQ}\"\n");
  // A set variable resolves, and the value is not logged.
  CHECK(r.config.serverKey.text == "FROM-THE-ENVIRONMENT-5" && r.config.serverKey.source == Source::kFile);
  CHECK(!EventsContain(r, "FROM-THE-ENVIRONMENT-5") && !EventsContain(r, "NEVR_QUEST_TEST_SET_VAR_ZXQ"));

  // ${VAR:?} that is unset rejects the whole file, as on the PC.
  r = Load(Full(), "auth:\n  http_key: \"${NEVR_QUEST_TEST_UNSET_VAR_ZXQ:?must be set}\"\n");
  CHECK(r.fileRejected);
  CHECK(EventsContain(r, "config file=config.yaml rejected reason=unset_required_variable (embedded defaults stay)"));
  CHECK(!EventsContain(r, "must be set") && !EventsContain(r, "NEVR_QUEST_TEST_UNSET_VAR_ZXQ"));
  ::unsetenv("NEVR_QUEST_TEST_SET_VAR_ZXQ");
}

void NoEventEverCarriesAConfiguredValue() {
  const LoadResult r = Load(Full(),
                            "auth:\n  http_key: \"FILE-SECRET-5\"\nservices:\n  socket_uri: \"wss://file.example/x\"\n"
                            "diagnostics:\n  hardware_dump: true\n");
  for (const char* secret : {kEmbSocket, kEmbHttp, kEmbApiKey, kEmbServerKey, "FILE-SECRET-5", "file.example"}) {
    CHECK(!EventsContain(r, secret));
  }
}

void AnEmptyFileValueCannotClearAnEmbeddedDefault() {
  const LoadResult r = Load(Full(), "auth:\n  http_key: \"\"\nservices:\n  socket_uri: \"\"\n");
  CHECK(r.config.httpKey.text == kEmbApiKey && r.config.httpKey.source == Source::kEmbedded);
  CHECK(r.config.socketUri.text == kEmbSocket);
  CHECK(EventsContain(r, "config file key=auth.http_key rejected reason=empty"));
}

void ConfigPathIsConfigYamlAndNeverGameConfigJson() {
  const std::string a = nevr_quest::ConfigFilePath("/sdcard/x/files");
  const std::string b = nevr_quest::ConfigFilePath("/sdcard/x/files/");
  CHECK(a == "/sdcard/x/files/config.yaml");
  CHECK(b == a);
  CHECK(a.find("config.json") == std::string::npos);
  CHECK(a.find("nevr-quest.json") == std::string::npos);
  CHECK(nevr_quest::ConfigFilePath("") == "config.yaml");
}

void RedirectIsGatedByActivation() {
  const std::string wsUrl = "wss://login.readyatdawn.com/rad/rad15_live";
  const std::string httpUrl = "https://config.readyatdawn.com/rad/rad15_live";

  // Nothing to redirect to: redirect is forced off and the game's own value stays.
  LoadResult off = nevr_quest::ResolveConfig(EmbeddedDefaults(), nullptr);
  CHECK(!nevr_quest::ResolveQuestRedirect(off.config, wsUrl, true, 5000).has_value());

  // An HTTP target only: redirect is on, the bridge is not.
  EmbeddedDefaults httpOnly;
  httpOnly.httpUri = kEmbHttp;
  LoadResult redirectOnly = nevr_quest::ResolveConfig(httpOnly, nullptr);
  CHECK(!nevr_quest::ResolveQuestRedirect(redirectOnly.config, wsUrl, true, 5000).has_value());
  const auto http = nevr_quest::ResolveQuestRedirect(redirectOnly.config, httpUrl, false, 0);
  CHECK(http.has_value() && *http == kEmbHttp);
  CHECK(!nevr_quest::ResolveQuestRedirect(redirectOnly.config, "https://other.example/x", false, 0).has_value());

  // The embedded endpoints with no file: redirect and bridge are on without any key saying so.
  LoadResult bridged = nevr_quest::ResolveConfig(Full(), nullptr);
  const auto loop = nevr_quest::ResolveQuestRedirect(bridged.config, wsUrl, true, 5000);
  CHECK(loop.has_value() && *loop == "ws://127.0.0.1:5000");
  const auto notReady = nevr_quest::ResolveQuestRedirect(bridged.config, wsUrl, false, 0);
  CHECK(notReady.has_value() && *notReady == kEmbSocket);
  const auto httpBridged = nevr_quest::ResolveQuestRedirect(bridged.config, httpUrl, true, 5000);
  CHECK(httpBridged.has_value() && *httpBridged == kEmbHttp);

  // A socket URI from the file replaces the embedded one.
  LoadResult fromFile = Load(Full(), "services:\n  socket_uri: \"wss://file.example/nevr\"\n");
  const auto fileWs = nevr_quest::ResolveQuestRedirect(fromFile.config, wsUrl, false, 0);
  CHECK(fileWs.has_value() && *fileWs == "wss://file.example/nevr");
}

}  // namespace

// The UI event probe (#391, #318): `diagnostics.ui_event_probe`, off unless the file says true, and it needs social.
void UiEventProbeIsOnlyEverOnByTheDiagnosticsKeyAndNeedsSocial() {
  {
    const LoadResult r = Load(Full(), "diagnostics:\n  ui_event_probe: true\n");
    CHECK(r.config.requested.uiEventProbe && r.config.effective.uiEventProbe);
    CHECK(nevr_quest::FeatureEnabled(r.config, Feature::kUiEventProbe));
    CHECK(EventsContain(r, "feature=ui_event_probe requested=on effective=on"));
    CHECK(!HasLevel(r, LogLevel::kWarn));
  }
  {
    const LoadResult r = Load(Full(), "social:\n  facade: false\ndiagnostics:\n  ui_event_probe: true\n");
    CHECK(r.config.requested.uiEventProbe && !r.config.effective.uiEventProbe);
    CHECK(EventsContain(r, "feature=ui_event_probe forced off reason=social_not_enabled"));
  }
  {
    const LoadResult r = Load(Full(), "diagnostics:\n  ui_event_probe: maybe\n");
    CHECK(!r.config.effective.uiEventProbe);
    CHECK(EventsContain(r, "config file key=diagnostics.ui_event_probe rejected reason=not_a_boolean"));
  }
  for (const char* text : {"diagnostics:\n  ui_event_probe: false\n", "social:\n  ui_event_probe: true\n",
                           "ui_event_probe: true\n", "diagnostics: {}\n"}) {
    const LoadResult r = Load(Full(), text);
    CHECK(!r.config.effective.uiEventProbe);
    CHECK(EventsContain(r, "feature=ui_event_probe requested=off effective=off"));
  }
  CHECK(std::string(nevr_quest::FeatureName(Feature::kUiEventProbe)) == "ui_event_probe");
}

int main() {
  DefaultsWithoutFile();
  NothingEmbeddedAndNoFileTurnsTheNetworkFeaturesOff();
  InvalidEmbeddedIsRejectedWithoutValue();
  FileOverridesPerKey();
  AnEmptyDocumentIsAFileThatChangesNothing();
  ASocketUriAloneTurnsRedirectAndBridgeOnAndLoginNeedsTheServerKey();
  FeatureDependenciesForceOff();
  SocialNeedsLoginAndResolvesLast();
  SocialFacadeFalseTurnsSocialAndItsNamesOffAndLeavesThePresenceObjectAlone();
  ABooleanThatIsNotOneKeepsItsDefaultAndIsRejectedByName();
  SendPresenceToMetaFalseIsTheLocalPresencePath();
  HardwareDumpIsOnlyEverOnByTheDiagnosticsKey();
  UiEventProbeIsOnlyEverOnByTheDiagnosticsKeyAndNeedsSocial();
  MalformedFileFallsBackToDefaults();
  ARejectedFileNeverEchoesWhatItHeld();
  AnUnknownKeyInsideAKnownSectionIsNotAnError();
  OversizedFileIsRejected();
  TheSizeCapIsInclusive();
  AliasesThatLoopOrExplodeAreRejectedNotWalkedForever();
  ADeepButLegalNestingStillLoads();
  BadKeyValuesKeepTheDefault();
  UriShapeValidation();
  AScalarOfAnyYamlTypeIsAValueLikeOnThePc();
  AnUnsetVariableInAKeyIsRejectedByKeyNotByName();
  NoEventEverCarriesAConfiguredValue();
  AnEmptyFileValueCannotClearAnEmbeddedDefault();
  ConfigPathIsConfigYamlAndNeverGameConfigJson();
  RedirectIsGatedByActivation();
  if (g_failures != 0) {
    std::fprintf(stderr, "quest_config_test: %d check(s) failed\n", g_failures);
    return 1;
  }
  std::printf("quest_config_test: all checks pass\n");
  return 0;
}
