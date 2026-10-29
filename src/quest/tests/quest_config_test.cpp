// Host and Android test for the Quest configuration and activation logic (quest_config.cpp). The file is
// config.yaml, parsed by the PC's nevr_config (src/core/nevr_config.cpp).
#include "core/nevr_config.h"
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

// Everything the file does not switch: redirect, bridge, login, social and the names on, the diagnostics, the local presence
// and the clock-level override (#928) off.
bool DefaultsOnly(const nevr_quest::Features& f) {
  return f.redirect && f.bridge && f.login && f.social && f.presenceNames && !f.clockLevels && !f.hwdump && !f.presenceLocal;
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
  // The invite/party tab event (#318) is on in a default config: it is what switches the tablet to Friends.
  CHECK(r.config.requested.uiEventProbe && r.config.effective.uiEventProbe);
  CHECK(EventsContain(r, "feature=ui_event_probe requested=on effective=on"));
  // The clock-level override (#928) is off with no file, and the feature report says so.
  CHECK(!r.config.requested.clockLevels && !r.config.effective.clockLevels);
  CHECK(!nevr_quest::FeatureEnabled(r.config, Feature::kClockLevels));
  CHECK(EventsContain(r, "feature=clock_levels requested=off effective=off"));
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

// The rad provider (#1220) is off unless diagnostics.pnsrad_provider asks for it, and it follows login: the rad provider
// sends no login of its own.
void PnsradProviderIsOnlyEverOnByItsKeyAndNeedsLogin() {
  {
    const LoadResult r = nevr_quest::ResolveConfig(Full(), nullptr);
    CHECK(!r.config.requested.pnsradProvider && !r.config.effective.pnsradProvider);
    CHECK(EventsContain(r, "feature=pnsrad_provider requested=off effective=off"));
  }
  {
    const LoadResult r = Load(Full(), "diagnostics:\n  pnsrad_provider: true\n");
    CHECK(r.config.requested.pnsradProvider && r.config.effective.pnsradProvider);
    CHECK(nevr_quest::FeatureEnabled(r.config, Feature::kPnsradProvider));
    CHECK(r.config.effective.login);
    CHECK(EventsContain(r, "feature=pnsrad_provider requested=on effective=on"));
    CHECK(!HasLevel(r, LogLevel::kWarn));
  }
  // No service target: no login, so the provider stays pnsovr and the reason is logged.
  {
    const LoadResult r = Load(EmbeddedDefaults(), "diagnostics:\n  pnsrad_provider: true\n");
    CHECK(r.config.requested.pnsradProvider && !r.config.effective.pnsradProvider);
    CHECK(!nevr_quest::FeatureEnabled(r.config, Feature::kPnsradProvider));
    CHECK(EventsContain(r, "pnsrad_provider"));
    CHECK(EventsContain(r, "login_not_enabled"));
  }
  {
    const LoadResult r = Load(Full(), "diagnostics:\n  pnsrad_provider: sure\n");
    CHECK(!r.config.effective.pnsradProvider);
    CHECK(EventsContain(r, "config file key=diagnostics.pnsrad_provider rejected reason=not_a_boolean"));
  }
  for (const char* text : {"diagnostics:\n  pnsrad_provider: false\n", "social:\n  pnsrad_provider: true\n",
                           "pnsrad_provider: true\n", "x-pnsrad_provider: true\n"}) {
    const LoadResult r = Load(Full(), text);
    CHECK(!r.config.effective.pnsradProvider);
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

// A rejection takes at most kMaxConfigNodes visits; the unbounded walk over 8 nested levels ran for 10.8 s. What
// proves the bound is the count, not the clock: TheNodeBudgetIsExactlyKMaxConfigNodes pins the visit at which a
// file is rejected, and the rejection reason below is only reachable past it. A wall-clock limit that a loaded
// host can meet cannot tell that host from a slow loop, so this one is a backstop for a walk that never ends
// (the rejection takes 64 ms on an idle host), not the check.
constexpr std::chrono::milliseconds kRejectionBackstop{5000};

LoadResult TimedLoad(const std::string& file, bool& withinBackstop) {
  const auto start = std::chrono::steady_clock::now();
  LoadResult r = Load(Full(), file);
  withinBackstop = std::chrono::steady_clock::now() - start < kRejectionBackstop;
  return r;
}

// A file the walk visits exactly `visits` nodes of, under the Quest's 64 KiB size cap: plain entries cannot reach
// kMaxConfigNodes in that size (OversizedFileIsRejected), so most visits come from aliases of a 10-node sequence.
// Visits: the root mapping (1), `x-a0`'s sequence and its 9 scalars (10), `x-b`'s sequence (1), each `*a0` in it
// (10 each, a visit of the aliased sequence and its scalars) and each plain `0` in it (1).
std::string FileWithVisits(std::size_t visits) {
  const std::size_t rest = visits - 12;
  std::string s = "x-a0: &a0 [0,0,0,0,0,0,0,0,0]\nx-b: [";
  for (std::size_t i = 0; i < rest / 10; ++i) s += i == 0 ? "*a0" : ",*a0";
  for (std::size_t i = 0; i < rest % 10; ++i) s += ",0";  // rest >= 10 for any real cap, so the list is not empty
  return s + "]\n";
}

// The walk counts every visit of a node, aliased or not, and rejects the file at the first visit past
// kMaxConfigNodes. So the file that visits exactly that many nodes is the last that loads and the next is the
// first that is rejected: a walk that counts a node twice, skips the count, or moves the cap changes which of the
// two loads. This, not the clock, is what proves the bound.
void TheNodeBudgetIsExactlyKMaxConfigNodes() {
  const char* const tooMany = "config file=config.yaml rejected reason=too_many_nodes (embedded defaults stay)";
  LoadResult r = Load(Full(), FileWithVisits(nevr::kMaxConfigNodes));
  CHECK(!r.fileRejected && !EventsContain(r, "too_many_nodes"));
  r = Load(Full(), FileWithVisits(nevr::kMaxConfigNodes + 1));
  CHECK(r.fileRejected && EventsContain(r, tooMany));
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

// The invite/party tab event (#391, #318): `diagnostics.ui_event_probe`, on unless the file says false; needs social.
void UiEventProbeIsOnByDefaultOffOnlyByTheDiagnosticsKeyAndNeedsSocial() {
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
    CHECK(r.config.effective.uiEventProbe);  // a rejected value keeps the default, which is on
    CHECK(EventsContain(r, "config file key=diagnostics.ui_event_probe rejected reason=not_a_boolean"));
  }
  {
    const LoadResult r = Load(Full(), "diagnostics:\n  ui_event_probe: false\n");
    CHECK(!r.config.requested.uiEventProbe && !r.config.effective.uiEventProbe);
    CHECK(EventsContain(r, "feature=ui_event_probe requested=off effective=off"));
  }
  {
    // The default follows the facade: a facade-off file never asked for the event, so nothing is forced off.
    const LoadResult r = Load(Full(), "social:\n  facade: false\n");
    CHECK(!r.config.requested.uiEventProbe && !r.config.effective.uiEventProbe);
    CHECK(EventsContain(r, "feature=ui_event_probe requested=off effective=off"));
    CHECK(!HasLevel(r, LogLevel::kWarn));
  }
  // Spellings the key does not read leave the default in place: on, with the social facade on.
  for (const char* text : {"social:\n  ui_event_probe: false\n", "ui_event_probe: false\n", "diagnostics: {}\n"}) {
    const LoadResult r = Load(Full(), text);
    CHECK(r.config.effective.uiEventProbe);
    CHECK(EventsContain(r, "feature=ui_event_probe requested=on effective=on"));
  }
  CHECK(std::string(nevr_quest::FeatureName(Feature::kUiEventProbe)) == "ui_event_probe");
}

// The texture quality (#943): `graphics.texture_quality`, one of seven words, texture_quality::kDefaultLevel unless the file names
// another. Every test of the absent key is written against kDefaultLevel (so the flip to `high` changes the pin test only);
// every test of a mistake is written against the literal `stock`, which must hold whatever the default is.
const char* DefaultWord() { return nevr_quest::texture_quality::LevelName(nevr_quest::texture_quality::kDefaultLevel); }

bool IsDefault(const LoadResult& r) {
  return r.config.textureQuality.level == nevr_quest::texture_quality::kDefaultLevel &&
         r.config.textureQuality.source != Source::kFile;
}

// A mistake: `stock` (the literal level, not the default), not from the file, one Warning that names the key and the fall back,
// and no Error (a rejected file logs its own Error line, so `rejectedFile` allows it).
bool IsStockFallback(const LoadResult& r, const std::string& reason, bool rejectedFile = false) {
  return r.config.textureQuality.level == nevr_quest::texture_quality::Level::kStock &&
         r.config.textureQuality.source != Source::kFile &&
         EventsWithAreAtLevel(r, "config file key=graphics.texture_quality rejected reason=" + reason, LogLevel::kWarn) &&
         EventsContain(r, "(falls back to stock;") && EventsContain(r, "config key=graphics.texture_quality quality=stock source=default") &&
         (rejectedFile || !HasLevel(r, LogLevel::kError));
}

void TextureQualityIsTheDefaultUnlessTheFileNamesAnotherLevel() {
  namespace tq = nevr_quest::texture_quality;
  {  // no file, an empty file, a file without the section, an empty section, a section with no value: the default, from the default
    const LoadResult none = nevr_quest::ResolveConfig(Full(), nullptr);
    CHECK(IsDefault(none));
    CHECK(EventsContain(none, std::string("config key=graphics.texture_quality quality=") + DefaultWord() + " source=default"));
    CHECK(!HasLevel(none, LogLevel::kWarn) && !HasLevel(none, LogLevel::kError));
    for (const char* text : {"", "version: 1\n", "graphics: {}\n", "graphics:\n", "graphics: ~\n"}) {
      const LoadResult r = Load(Full(), text);
      CHECK(IsDefault(r));
      CHECK(EventsContain(r, std::string("config key=graphics.texture_quality quality=") + DefaultWord() + " source=default"));
      CHECK(!HasLevel(r, LogLevel::kWarn));
    }
  }
  const struct {
    const char* word;
    tq::Level level;
  } kWords[] = {{"stock", tq::Level::kStock},       {"very_low", tq::Level::kVeryLow}, {"low", tq::Level::kLow},
                {"medium", tq::Level::kMedium},     {"high", tq::Level::kHigh},         {"very_high", tq::Level::kVeryHigh},
                {"ultra", tq::Level::kUltra}};
  for (const auto& w : kWords) {  // each word, plain and quoted
    for (const char* quote : {"", "\""}) {
      const LoadResult r = Load(Full(), std::string("graphics:\n  texture_quality: ") + quote + w.word + quote + "\n");
      CHECK(r.config.textureQuality.level == w.level && r.config.textureQuality.source == Source::kFile);
      CHECK(EventsContain(r, std::string("config key=graphics.texture_quality quality=") + w.word + " source=config"));
      CHECK(!HasLevel(r, LogLevel::kWarn) && !HasLevel(r, LogLevel::kError));
      CHECK(std::string(nevr_quest::TextureSourceName(r.config.textureQuality)) == "config");
      CHECK(DefaultsOnly(r.config.requested) && DefaultsOnly(r.config.effective));  // no feature moves
    }
  }
  {  // a file that is rejected as a whole contributes nothing, this key included, and the answer is a mistake's: `stock`
    const LoadResult r = Load(Full(), "graphics:\n  texture_quality: ultra\nnot_a_section: 1\n");
    CHECK(r.fileRejected);
    CHECK(IsStockFallback(r, "file_rejected", true));
    CHECK(EventsContain(r, "config key=graphics.texture_quality quality=stock source=default"));
  }
  for (const char* text : {"graphics: [unclosed\n", "graphics:\n  texture_quality: ultra\n\tbad: tab\n", "{{{\n", "- a\n- b\n"}) {
    const LoadResult r = Load(Full(), text);  // unparseable, or a top level that is not a mapping
    CHECK(r.fileRejected);
    CHECK(IsStockFallback(r, "file_rejected", true));
  }
  // Only this key in this section: the same word anywhere else is nothing (the default, no warning) unless it makes the file
  // unknown to the loader as a whole, which is a mistake like any other. (An unknown key inside `graphics` is the next test.)
  for (const char* text : {"texture_quality: ultra\n", "diagnostics:\n  texture_quality: ultra\n",
                           "x-graphics:\n  texture_quality: ultra\n"}) {
    const LoadResult r = Load(Full(), text);
    if (r.fileRejected) {  // an unknown top-level key rejects the file as a whole: the mistake above
      CHECK(IsStockFallback(r, "file_rejected", true));
    } else {
      CHECK(IsDefault(r));
      // #583: an unknown key under a known section (`diagnostics`) is ignored and counted into one Warning; the
      // other two spellings are not under a known section and warn of nothing.
      if (std::string(text).rfind("diagnostics:", 0) == 0) {
        CHECK(EventsContain(r, "unknown_nested=1"));
      } else {
        CHECK(!HasLevel(r, LogLevel::kWarn));
      }
    }
  }
}

// #583: a duplicate key and an unknown key under a known section are counted into one Warning that names no key and no
// value; a file with neither logs no count line.
void DuplicateAndUnknownKeysAreCountedInOneWarning() {
  {
    const LoadResult r = Load(Full(), "social:\n  facade: true\n  facade: false\n  send_presense: false\n");
    CHECK(!r.fileRejected);
    CHECK(EventsWithAreAtLevel(r, "duplicates=1 unknown_nested=1 (", LogLevel::kWarn));
    CHECK(!EventsContain(r, "send_presense"));
    CHECK(CountLevel(r, LogLevel::kWarn) == 1);
  }
  {
    const LoadResult r = Load(Full(), "social:\n  send_presence_to_meta: false\n");
    CHECK(!EventsContain(r, "duplicates="));
    CHECK(!EventsContain(r, "unknown_nested="));
  }
}

// Any key under `graphics` other than texture_quality is named in a Warning, so a misspelling or another case is heard
// instead of silently giving the default (which a later change of the default would turn into a silent change of level).
// The level is not touched by it: the key is absent, so it is the default.
void AnUnknownKeyUnderGraphicsWarnsAndNamesTheKey() {
  for (const char* spelling : {"texture_qualty", "Texture_Quality", "texture-quality", "quality", "texturequality", "texture_quality "}) {
    const LoadResult r = Load(Full(), std::string("graphics:\n  \"") + spelling + "\": ultra\n");
    CHECK(!r.fileRejected);
    CHECK(IsDefault(r));  // never applied, never stock by force: the default, as for an absent key
    CHECK(EventsWithAreAtLevel(r, std::string("config file key=graphics rejected reason=unknown_key name=") +
                                      (std::string(spelling) == "texture_quality " ? "(not printable ASCII without spaces)" : spelling) + " (",
                               LogLevel::kWarn));
    CHECK(EventsContain(r, "the only key under graphics is texture_quality"));
    CHECK(!HasLevel(r, LogLevel::kError));
  }
  {  // the real key beside a misspelt one: the real one applies, the other is named
    const LoadResult r = Load(Full(), "graphics:\n  texture_quality: low\n  texture_qualty: ultra\n");
    CHECK(r.config.textureQuality.level == nevr_quest::texture_quality::Level::kLow && r.config.textureQuality.source == Source::kFile);
    CHECK(EventsWithAreAtLevel(r, "reason=unknown_key name=texture_qualty (", LogLevel::kWarn));
    CHECK(!EventsContain(r, "name=texture_quality ("));
  }
  {  // the real key alone, an empty section, other sections' keys: no such warning
    for (const char* text : {"graphics:\n  texture_quality: low\n", "graphics: {}\n", "diagnostics:\n  quality: 1\n"}) {
      CHECK(!EventsContain(Load(Full(), text), "unknown_key"));
    }
  }
  {  // a long or odd name is described, never echoed
    const LoadResult r = Load(Full(), "graphics:\n  " + std::string(40, 'k') + ": 1\n");
    CHECK(EventsContain(r, "reason=unknown_key name=(longer than 32 bytes) ("));
    CHECK(!EventsContain(r, std::string(40, 'k')));
  }
  {  // a file full of them cannot flood the log: 8 are named, one line counts the rest
    std::string yaml = "graphics:\n";
    for (int i = 0; i < 20; ++i) yaml += "  k" + std::to_string(i) + ": 1\n";
    const LoadResult r = Load(Full(), yaml);
    int named = 0;
    for (const auto& e : r.events) {
      if (e.message.find("reason=unknown_key name=") != std::string::npos) ++named;
    }
    CHECK(named == 8);
    CHECK(EventsWithAreAtLevel(r, "reason=unknown_key more=12 (ignored)", LogLevel::kWarn));
    const LoadResult atCap = Load(Full(), "graphics:\n  a: 1\n  b: 1\n  c: 1\n  d: 1\n  e: 1\n  f: 1\n  g: 1\n  h: 1\n");
    CHECK(!EventsContain(atCap, "more="));
  }
}

void ATextureQualityThatIsNotALevelFallsBackToStockAndNamesTheWord() {
  namespace tq = nevr_quest::texture_quality;
  for (const char* bad : {"ultra-high", "High", "HIGH", "Stock", "ULTRA", "very high", "veryhigh", "none", "0", "3", "true", "default", "native"}) {
    const std::string yaml = std::string("graphics:\n  texture_quality: \"") + bad + "\"\n";
    const LoadResult r = Load(Full(), yaml);
    CHECK(!r.fileRejected);
    // A word with a space in it is named by what is wrong with it; the rest by themselves.
    const std::string named = std::string(bad) == "very high" ? "(not printable ASCII without spaces)" : bad;
    CHECK(IsStockFallback(r, "unknown_level value=" + named + " ("));
  }
  for (const char* number : {"0", "3", "5", "1.5", "-1", "true", "null"}) {  // a bare scalar of another YAML type
    const LoadResult r = Load(Full(), std::string("graphics:\n  texture_quality: ") + number + "\n");
    CHECK(!r.fileRejected);
    CHECK(r.config.textureQuality.level == tq::Level::kStock && r.config.textureQuality.source != Source::kFile);
    CHECK(EventsWithAreAtLevel(r, "config file key=graphics.texture_quality rejected reason=", LogLevel::kWarn));
    CHECK(EventsContain(r, "(falls back to stock;"));
  }
  {  // the word is named only when it is short printable ASCII; otherwise what is wrong with it
    const std::string longWord(33, 'x');
    const LoadResult r = Load(Full(), "graphics:\n  texture_quality: " + longWord + "\n");
    CHECK(EventsContain(r, "reason=unknown_level value=(longer than 32 bytes)"));
    CHECK(!EventsContain(r, longWord));
    const std::string edge(32, 'y');
    const LoadResult atLimit = Load(Full(), "graphics:\n  texture_quality: " + edge + "\n");
    CHECK(EventsContain(atLimit, "reason=unknown_level value=" + edge + " ("));
    const LoadResult control = Load(Full(), "graphics:\n  texture_quality: \"hi\\tgh\"\n");
    CHECK(EventsContain(control, "reason=unknown_level value=(not printable ASCII without spaces)"));
    const LoadResult accent = Load(Full(), "graphics:\n  texture_quality: \"h\\u00efgh\"\n");
    CHECK(EventsContain(accent, "reason=unknown_level value=(not printable ASCII without spaces)"));
    CHECK(!EventsContain(accent, "\xc3"));
  }
  {  // empty: double-quoted and single-quoted
    for (const char* text : {"graphics:\n  texture_quality: \"\"\n", "graphics:\n  texture_quality: ''\n"}) {
      const LoadResult r = Load(Full(), text);
      CHECK(!r.fileRejected);
      CHECK(IsStockFallback(r, "unknown_level value=(empty) ("));
    }
  }
  ::unsetenv("NEVR_QUEST_TEST_UNSET_VAR_ZXQ");
  for (const char* text : {"graphics:\n  texture_quality: \"${NEVR_QUEST_TEST_UNSET_VAR_ZXQ}\"\n",
                           "graphics:\n  texture_quality: ${NEVR_QUEST_TEST_UNSET_VAR_ZXQ}\n"}) {
    // an unset ${VAR}, quoted and unquoted, is rejected by key, never by name
    const LoadResult r = Load(Full(), text);
    CHECK(IsStockFallback(r, "unset_variable"));
    CHECK(!EventsContain(r, "value=${") && !EventsContain(r, "ZXQ") && !EventsContain(r, "unknown_level"));
  }
  {  // a bad word does not disturb the other keys of the file
    const LoadResult r = Load(Full(), "graphics:\n  texture_quality: bogus\ndiagnostics:\n  hardware_dump: true\n");
    CHECK(r.config.requested.hwdump);
    CHECK(r.config.textureQuality.level == tq::Level::kStock);
  }
}

// A value that is not a word at all: the typed getters read none of these as a string, the same as an absent key, so the
// reader asks the config what kind of node it is (NevrConfig::Kind).
void ATextureQualityThatIsNotAWordFallsBackToStock() {
  for (const char* text : {"graphics:\n  texture_quality:\n    nested: high\n",   // a mapping
                           "graphics:\n  texture_quality: {}\n",                    // an empty mapping
                           "graphics:\n  texture_quality: [high]\n",                // a list
                           "graphics:\n  texture_quality:\n    - high\n    - ultra\n",
                           "graphics:\n  texture_quality: []\n",                    // an empty list
                           "graphics:\n  texture_quality:\n",                       // a key with no value
                           "graphics:\n  texture_quality: ~\n",
                           "graphics:\n  texture_quality: null\n"}) {
    const LoadResult r = Load(Full(), text);
    CHECK(!r.fileRejected);
    CHECK(IsStockFallback(r, "not_a_word"));
  }
}

// `graphics` itself is not a mapping, so the key under it cannot be read: a mistake, since the file meant to say something.
void AGraphicsSectionThatIsNotAMappingFallsBackToStock() {
  for (const char* text : {"graphics: ultra\n", "graphics: \"\"\n", "graphics: 3\n", "graphics: [ultra]\n",
                           "graphics:\n  - texture_quality: ultra\n"}) {
    const LoadResult r = Load(Full(), text);
    CHECK(!r.fileRejected);
    CHECK(IsStockFallback(r, "graphics_not_a_mapping"));
  }
}

// The default is for an absent key only. Every mistake above gives `stock`, so with the default flipped to `high` a typo still
// does not turn on the level that costs the most: the two constants are separate and this pins that they stay so.
void AMistakeNeverUsesTheDefault() {
  namespace tq = nevr_quest::texture_quality;
  CHECK(tq::kFallbackLevel == tq::Level::kStock);
  CHECK(tq::SizeFor(tq::kFallbackLevel, tq::Headset::kQuest3).width == 0);  // no hook, so no eye size
}

// The clock-level override (#928): `diagnostics.clock_levels_override`, off unless the file says true, with no prerequisite.
// It is a key under `diagnostics`: a `features` section is an unknown top-level key and rejects the whole file.
void ClockLevelsOverrideIsOffUnlessTheDiagnosticsKeyIsTrue() {
  for (const char* word : {"true", "True", "TRUE", "yes", "Yes", "on", "On", "1"}) {
    const LoadResult r = Load(Full(), std::string("diagnostics:\n  clock_levels_override: ") + word + "\n");
    CHECK(!r.fileRejected);
    CHECK(r.config.requested.clockLevels && r.config.effective.clockLevels);
    CHECK(nevr_quest::FeatureEnabled(r.config, Feature::kClockLevels));
    CHECK(EventsWithAreAtLevel(r, "feature=clock_levels requested=on effective=on", LogLevel::kInfo));
    CHECK(r.config.effective.redirect && r.config.effective.social);  // nothing else moved
  }
  for (const char* word : {"false", "False", "no", "off", "0"}) {
    const LoadResult r = Load(Full(), std::string("diagnostics:\n  clock_levels_override: ") + word + "\n");
    CHECK(!r.config.effective.clockLevels);
    // The resolved value is logged at Info even when off.
    CHECK(EventsContain(r, "feature=clock_levels requested=off effective=off"));
  }
  {  // it needs nothing else: with no service target at all it is still on when asked, and absent it is off
    CHECK(Load(EmbeddedDefaults(), "diagnostics:\n  clock_levels_override: true\n").config.effective.clockLevels);
    CHECK(!Load(EmbeddedDefaults(), "diagnostics: {}\n").config.effective.clockLevels);
  }
  {  // an unusable value keeps the default (off), is rejected by key and is never echoed
    LoadResult r = Load(Full(), "diagnostics:\n  clock_levels_override: maybe\n");
    CHECK(!r.fileRejected && !r.config.effective.clockLevels);
    CHECK(EventsWithAreAtLevel(r, "config file key=diagnostics.clock_levels_override rejected reason=not_a_boolean",
                               LogLevel::kWarn));
    CHECK(!EventsContain(r, "maybe"));
  }
  // Anywhere but diagnostics.clock_levels_override leaves the default (off).
  for (const char* text : {"social:\n  clock_levels_override: true\n", "x-clock_levels_override: true\n",
                           "diagnostics:\n  clock_levels: true\n", "diagnostics: {}\n"}) {
    const LoadResult r = Load(Full(), text);
    CHECK(!r.config.effective.clockLevels);
    CHECK(EventsContain(r, "feature=clock_levels requested=off effective=off"));
  }
  {  // a `features` section is an unknown top-level key: the file is rejected and the default (off) stays
    const LoadResult r = Load(Full(), "features:\n  clock_levels_override: true\n");
    CHECK(r.fileRejected && !r.config.effective.clockLevels);
  }
  CHECK(std::string(nevr_quest::FeatureName(Feature::kClockLevels)) == "clock_levels");
}

// #438: the file's `plugins:` list reaches the sentinel as the specs the PC parser read. The sentinel plans from them.
void ThePluginsListIsKeptInOrderWithItsFlagsAndArgs() {
  const LoadResult r = Load(Full(), R"YAML(
plugins:
  - name: first
    file: libfirst.so
    required: true
    early: true
    args:
      level: 3
  - name: second
    enabled: false
)YAML");
  CHECK(!r.fileRejected);
  CHECK(r.config.plugins.size() == 2);
  if (r.config.plugins.size() != 2) return;
  CHECK(r.config.plugins[0].name == "first");
  CHECK(r.config.plugins[0].file == "libfirst.so");
  CHECK(r.config.plugins[0].required);
  CHECK(r.config.plugins[0].early);
  CHECK(r.config.plugins[0].args.at("level") == "3");
  CHECK(r.config.plugins[1].name == "second");
  CHECK(!r.config.plugins[1].enabled);
}

void NoPluginsKeyAndAnAbsentFileGiveNoPlugins() {
  CHECK(Load(Full(), "social:\n  facade: true\n").config.plugins.empty());
  CHECK(nevr_quest::ResolveConfig(Full(), nullptr).config.plugins.empty());
}

// A file the sentinel rejects as a whole contributes nothing, plugins included: a half-read list never loads code.
void ARejectedFileGivesNoPlugins() {
  const LoadResult r = Load(Full(), "plugins:\n  - name: first\n  bad: [unclosed\n");
  CHECK(r.fileRejected);
  CHECK(r.config.plugins.empty());
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
  PnsradProviderIsOnlyEverOnByItsKeyAndNeedsLogin();
  UiEventProbeIsOnByDefaultOffOnlyByTheDiagnosticsKeyAndNeedsSocial();
  TextureQualityIsTheDefaultUnlessTheFileNamesAnotherLevel();
  ATextureQualityThatIsNotALevelFallsBackToStockAndNamesTheWord();
  ATextureQualityThatIsNotAWordFallsBackToStock();
  AGraphicsSectionThatIsNotAMappingFallsBackToStock();
  AnUnknownKeyUnderGraphicsWarnsAndNamesTheKey();
  DuplicateAndUnknownKeysAreCountedInOneWarning();
  AMistakeNeverUsesTheDefault();
  ClockLevelsOverrideIsOffUnlessTheDiagnosticsKeyIsTrue();
  MalformedFileFallsBackToDefaults();
  ARejectedFileNeverEchoesWhatItHeld();
  AnUnknownKeyInsideAKnownSectionIsNotAnError();
  OversizedFileIsRejected();
  TheSizeCapIsInclusive();
  AliasesThatLoopOrExplodeAreRejectedNotWalkedForever();
  TheNodeBudgetIsExactlyKMaxConfigNodes();
  ADeepButLegalNestingStillLoads();
  BadKeyValuesKeepTheDefault();
  UriShapeValidation();
  AScalarOfAnyYamlTypeIsAValueLikeOnThePc();
  AnUnsetVariableInAKeyIsRejectedByKeyNotByName();
  NoEventEverCarriesAConfiguredValue();
  AnEmptyFileValueCannotClearAnEmbeddedDefault();
  ConfigPathIsConfigYamlAndNeverGameConfigJson();
  RedirectIsGatedByActivation();
  ThePluginsListIsKeptInOrderWithItsFlagsAndArgs();
  NoPluginsKeyAndAnAbsentFileGiveNoPlugins();
  ARejectedFileGivesNoPlugins();
  if (g_failures != 0) {
    std::fprintf(stderr, "quest_config_test: %d check(s) failed\n", g_failures);
    return 1;
  }
  std::printf("quest_config_test: all checks pass\n");
  return 0;
}
