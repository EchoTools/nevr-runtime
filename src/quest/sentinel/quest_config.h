// Quest client configuration and feature activation: pure, no Android or game headers, so the
// host test and the NDK build compile this one source.
//
// Precedence per key: file value, else the value embedded at build time, else absent. The file is
// `nevr-quest.json` in the app's external files directory; it is never `config.json`, never a
// `.env`, and nothing here reads the environment. Features (redirect, bridge, login, social) are off
// unless the build turns them on (EmbeddedDefaults::features) or the file does, and a feature whose prerequisite is off or missing is forced off
// with a logged reason. A value in the file that is invalid (including the empty string) is
// rejected and the embedded default stays: the file cannot clear a default. A key that appears
// twice in one object takes its last value and logs a warning.
#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

namespace nevr_quest {

inline constexpr const char* kConfigFileName = "nevr-quest.json";
inline constexpr std::size_t kMaxConfigBytes = 64 * 1024;

enum class Feature { kRedirect, kBridge, kLogin, kSocial, kHwDump, kObbSkip, kPresenceNames, kPresenceLocal, kSelfCheck };

enum class Source { kAbsent, kEmbedded, kFile };

enum class LogLevel { kInfo, kWarn, kError };

// What the build embedded (empty string = nothing embedded for that key).
struct EmbeddedDefaults {
  const char* socketUri = "";
  const char* httpUri = "";
  const char* httpKey = "";
  const char* serverKey = "";
  // Comma-separated feature names the build turns on when the file does not say otherwise (a release
  // candidate: the APK logs in with no config file). Empty: every feature is off until the file turns it on.
  const char* features = "";
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
  // The hardware/environment dump (#335): a diagnostic build only, never on unless the file asks for it.
  bool hwdump = false;
  // Report the OBB mount as done and use the data root the game falls back to (#319): no ~30 s wait in
  // CSysFile::Init. Off until a headset run confirms it; independent of every other feature.
  bool obbSkip = false;
  // Answer the rich presence's destination lookup from a built-in table ("Social Lobby", "Arena", ...) when
  // Meta's GetDestinations failed (#393): the status under the player's name stops reading the game's second
  // field. Off until a headset run confirms it; needs the social facade (the status travels in its member data).
  bool presenceNames = false;
  // Rich presence is not sent to Meta's platform service (#396, option b of #393): the presence object's
  // ShareData, RefreshDestinations and Clear are answered locally, so no group_presence request leaves (they
  // fail for this APK's identity anyway). Friends' status comes from the game service, which derives it from
  // the match. Off until a headset run confirms it; needs the social facade.
  bool presenceLocal = false;
  // Report the run-card checks as remote logs to the game service (runtime/compat/self_check.h) and connect
  // with debug=true so the service asks the game for every log category. A release candidate turns it on;
  // needs the login (the results go out on the login connection).
  bool selfCheck = false;
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
  Features requested;  // what the file asked for (all off without a file)
  Features effective;  // requested, after prerequisite checks
};

struct LoadResult {
  ResolvedConfig config;
  bool fileRejected = false;  // the file existed but was not usable as a whole
  std::vector<LogEvent> events;
};

// `fileText` is null when the file does not exist. Never throws on malformed input.
LoadResult ResolveConfig(const EmbeddedDefaults& defaults, const std::string* fileText);

const char* LevelName(LogLevel level);

// One line of the on-disk log: a single JSON object ending in '\n', no ANSI, valid UTF-8
// (invalid bytes in `message` are replaced).
std::string FormatDiskLogLine(LogLevel level, long long unixMs, const std::string& message);

// `<filesDir>/nevr-quest.json`, tolerant of a trailing slash.
std::string ConfigFilePath(const std::string& filesDir);

bool FeatureEnabled(const ResolvedConfig& config, Feature feature);
const char* FeatureName(Feature feature);
const char* SourceName(Source source);

// The shared redirect policy (runtime/lifecycle/service_redirect) gated by the redirect and bridge
// features. nullopt leaves the game's original value untouched.
std::optional<std::string> ResolveQuestRedirect(const ResolvedConfig& config, const std::string& url,
                                           bool bridgeReady, unsigned bridgePort);

}  // namespace nevr_quest
