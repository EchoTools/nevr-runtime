// Quest client configuration and feature activation: pure, no Android or game headers, so the
// host test and the NDK build compile this one source.
//
// The file is `config.yaml` in the app's external files directory: the same file format and parser as the PC
// (nevr_config, src/core/nevr_config.cpp), so the keys are the PC's keys (docs/reference/example-config.yaml).
// It is never `config.json`, never a `.env`, and nothing here reads the environment (the `${VAR}`
// interpolation in a value is nevr_config's, as on the PC).
//
// Precedence per key: file value, else the value embedded at build time, else absent. A value in the file that
// is invalid (including the empty string) is rejected and the embedded default stays: the file cannot clear a
// default. A file that is unusable as a whole (a YAML error, an unknown top-level key, a top level that is not a
// mapping, a malformed `plugins` list, an unset `${VAR:?}`, nesting or alias expansion past nevr_config's bounds,
// too large) contributes nothing: the embedded defaults stay and one Error line names the file and why.
//
// Features have no switch of their own except where the PC has a key. Redirect is on when a target resolves
// (`services.socket_uri` or `auth.http_uri`), the bridge when the redirect is on and `services.socket_uri`
// resolves, login when the bridge is on and `auth.server_key` resolves; the social facade is on unless
// `social.facade` is false (PC's key and default) and still needs login here; its status names ride with it.
// `social.send_presence_to_meta: false` answers the rich presence locally; `diagnostics.hardware_dump: true`
// turns the hardware dump on. A feature whose prerequisite is off or missing is forced off with a logged reason.
#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

namespace nevr_quest {

inline constexpr const char* kConfigFileName = "config.yaml";
// A file of this name in the files directory is not read; the sentinel reports once that it is present and ignored.
inline constexpr const char* kLegacyConfigFileName = "nevr-quest.json";
inline constexpr std::size_t kMaxConfigBytes = 64 * 1024;

enum class Feature { kRedirect, kBridge, kLogin, kSocial, kHwDump, kPresenceNames, kPresenceLocal };

enum class Source { kAbsent, kEmbedded, kFile };

enum class LogLevel { kInfo, kWarn, kError };

// What the build embedded (empty string = nothing embedded for that key).
struct EmbeddedDefaults {
  const char* socketUri = "";
  const char* httpUri = "";
  const char* httpKey = "";
  const char* serverKey = "";
};

struct Value {
  std::string text;
  Source source = Source::kAbsent;
};

struct Features {
  bool redirect = false;
  bool bridge = false;
  bool login = false;
  bool social = false;
  // The hardware/environment dump (#335): `diagnostics.hardware_dump`, never on unless the file asks for it.
  bool hwdump = false;
  // Answer the rich presence's destination lookup from a built-in table ("Social Lobby", "Arena", ...) when
  // Meta's GetDestinations failed (#393): the status under the player's name stops reading the game's second
  // field. On whenever the social facade is (it has no key); needs the facade (the status travels in its member
  // data).
  bool presenceNames = false;
  // Rich presence is not sent to Meta's platform service (#396, option b of #393): the presence object's
  // ShareData, RefreshDestinations and Clear are answered locally, so no group_presence request leaves (they
  // fail for this APK's identity anyway). Friends' status comes from the game service, which derives it from
  // the match. `social.send_presence_to_meta: false` turns it on; needs the social facade.
  bool presenceLocal = false;
};

struct LogEvent {
  LogLevel level;
  std::string message;  // never contains a configured value, only key and feature names
};

struct ResolvedConfig {
  Value socketUri;
  Value httpUri;
  Value httpKey;
  Value serverKey;
  Features requested;  // what the file and the defaults ask for
  Features effective;  // requested, after prerequisite checks
};

struct LoadResult {
  ResolvedConfig config;
  bool fileRejected = false;  // the file existed but was not usable as a whole
  std::vector<LogEvent> events;
};

// `fileText` is null when the file does not exist. `filePath` is only what the log lines name. Never throws:
// every exception the YAML parser raises is caught here, by name, and becomes a rejected file.
LoadResult ResolveConfig(const EmbeddedDefaults& defaults, const std::string* fileText,
                         const std::string& filePath = kConfigFileName);

// `<filesDir>/config.yaml` and `<filesDir>/nevr-quest.json`, tolerant of a trailing slash.
std::string ConfigFilePath(const std::string& filesDir);
std::string LegacyConfigFilePath(const std::string& filesDir);

bool FeatureEnabled(const ResolvedConfig& config, Feature feature);
const char* FeatureName(Feature feature);
const char* SourceName(Source source);

// The shared redirect policy (runtime/lifecycle/service_redirect) gated by the redirect and bridge
// features. nullopt leaves the game's original value untouched.
std::optional<std::string> ResolveQuestRedirect(const ResolvedConfig& config, const std::string& url,
                                           bool bridgeReady, unsigned bridgePort);

}  // namespace nevr_quest
