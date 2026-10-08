// Quest client configuration and feature activation: pure, no Android or game headers, so the
// host test and the NDK build compile this one source.
//
// Precedence per key: file value, else the value embedded at build time, else absent. The file is
// `nevr-quest.json` in the app's external files directory; it is never `config.json`, never a
// `.env`, and nothing here reads the environment. Features (redirect, bridge, login) are off
// unless the file turns them on, and a feature whose prerequisite is off or missing is forced off
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

enum class Feature { kRedirect, kBridge, kLogin };

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
