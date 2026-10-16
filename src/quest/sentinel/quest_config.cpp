#include "quest/sentinel/quest_config.h"

#include "core/nevr_config.h"
#include "runtime/lifecycle/service_redirect.h"

#include <nlohmann/json.hpp>

#include <array>
#include <exception>
#include <optional>
#include <string_view>
#include <utility>

namespace nevr_quest {
namespace {

constexpr std::size_t kMaxUriBytes = 2048;
constexpr std::size_t kMaxKeyBytes = 512;

enum class Kind { kSocketUri, kHttpUri, kSecret };

struct KeySpec {
  const char* name;
  Kind kind;
  Value ResolvedConfig::*slot;
};

// The config.yaml paths: the PC's keys (src/runtime/lifecycle/service_map.cpp kMap).
constexpr std::array<KeySpec, 4> kKeys = {{
    {"services.socket_uri", Kind::kSocketUri, &ResolvedConfig::socketUri},
    {"auth.http_uri", Kind::kHttpUri, &ResolvedConfig::httpUri},
    {"auth.http_key", Kind::kSecret, &ResolvedConfig::httpKey},
    {"auth.server_key", Kind::kSecret, &ResolvedConfig::serverKey},
}};

// The boolean keys. A missing or unusable value keeps the default.
constexpr const char* kSocialFacadeKey = "social.facade";                      // PC's key and default (true)
constexpr const char* kSendPresenceToMetaKey = "social.send_presence_to_meta";  // Quest only (default true)
constexpr const char* kHardwareDumpKey = "diagnostics.hardware_dump";          // Quest only (default false)

struct FeatureSpec {
  const char* name;
  Feature feature;
  bool Features::*flag;
};

constexpr std::array<FeatureSpec, 7> kFeatures = {{
    {"redirect", Feature::kRedirect, &Features::redirect},
    {"bridge", Feature::kBridge, &Features::bridge},
    {"login", Feature::kLogin, &Features::login},
    {"social", Feature::kSocial, &Features::social},
    {"hwdump", Feature::kHwDump, &Features::hwdump},
    {"presence_names", Feature::kPresenceNames, &Features::presenceNames},
    {"presence_local", Feature::kPresenceLocal, &Features::presenceLocal},
}};

bool HasControlOrSpace(std::string_view s) {
  for (const char ch : s) {
    const auto u = static_cast<unsigned char>(ch);
    if (u <= 0x20 || u == 0x7f) return true;
  }
  return false;
}

bool StartsWith(std::string_view s, std::string_view prefix) {
  return s.size() >= prefix.size() && s.substr(0, prefix.size()) == prefix;
}

// Returns an empty string when valid, else a reason that carries no part of the value.
const char* ValidateValue(Kind kind, std::string_view v) {
  if (v.empty()) return "empty";
  if (HasControlOrSpace(v)) return "contains whitespace or a control character";
  if (kind == Kind::kSecret) return v.size() > kMaxKeyBytes ? "too long" : "";
  if (v.size() > kMaxUriBytes) return "too long";
  std::string_view rest;
  if (kind == Kind::kSocketUri) {
    if (StartsWith(v, "wss://")) rest = v.substr(6);
    else if (StartsWith(v, "ws://")) rest = v.substr(5);
    else return "scheme must be ws:// or wss://";
  } else {
    if (StartsWith(v, "https://")) rest = v.substr(8);
    else if (StartsWith(v, "http://")) rest = v.substr(7);
    else return "scheme must be http:// or https://";
  }
  if (rest.empty() || rest.front() == '/' || rest.front() == '?' || rest.front() == '#') {
    return "missing host";
  }
  return "";
}

void Add(LoadResult& r, LogLevel level, std::string message) {
  r.events.push_back({level, std::move(message)});
}

// `events` is null when re-applying defaults after a file rejection (the warnings were already logged).
void ApplyEmbedded(ResolvedConfig& config, const EmbeddedDefaults& d, std::vector<LogEvent>* events) {
  const char* const values[] = {d.socketUri, d.httpUri, d.httpKey, d.serverKey};
  for (std::size_t i = 0; i < kKeys.size(); ++i) {
    const char* raw = values[i] == nullptr ? "" : values[i];
    if (raw[0] == '\0') continue;
    const char* bad = ValidateValue(kKeys[i].kind, raw);
    if (bad[0] != '\0') {
      if (events != nullptr) {
        events->push_back({LogLevel::kWarn, std::string("embedded default rejected key=") +
                                                kKeys[i].name + " reason=" + bad});
      }
      continue;
    }
    (config.*kKeys[i].slot) = {raw, Source::kEmbedded};
  }
}

// What the build and an absent or rejected file give: the network features follow from the targets (Derive checks
// them), the social facade and its names are on, the local presence and the hardware dump are off.
void SetDefaultRequests(ResolvedConfig& config) {
  config.requested.redirect = true;
  config.requested.bridge = true;
  config.requested.login = true;
  config.requested.social = true;
  config.requested.presenceNames = true;
  config.requested.presenceLocal = false;
  config.requested.hwdump = false;
}

// A boolean key: absent keeps `fallback`; an unusable value keeps it too and is rejected by key, never by value.
bool ReadBool(const nevr::NevrConfig& cfg, const char* key, bool fallback, LoadResult& r) {
  bool unsetBare = false;
  const std::optional<std::string> text = cfg.GetString(key, &unsetBare);
  if (!text) return fallback;
  if (unsetBare) {
    Add(r, LogLevel::kWarn, std::string("config file key=") + key + " rejected reason=unset_variable (default stays)");
    return fallback;
  }
  const std::optional<bool> value = cfg.GetBool(key);
  if (!value) {
    Add(r, LogLevel::kWarn, std::string("config file key=") + key + " rejected reason=not_a_boolean (default stays)");
    return fallback;
  }
  return *value;
}

// Reads every key from a loaded config. A throw (nevr_config's, or yaml-cpp's through it) propagates to ApplyFile.
void ApplyLoaded(LoadResult& r, const nevr::NevrConfig& cfg, const std::string& path) {
  if (!cfg.UnsetBareVars().empty()) {
    // Count only: the name is the file's own word. The key lines below say which of ours it hit.
    Add(r, LogLevel::kWarn,
        "config file=" + path + " unset_variable_references=" + std::to_string(cfg.UnsetBareVars().size()) +
            " (the text is kept as written; a key that uses one counts as unset)");
  }
  for (const KeySpec& k : kKeys) {
    bool unsetBare = false;
    const std::optional<std::string> value = cfg.GetString(k.name, &unsetBare);
    if (!value) continue;
    if (unsetBare) {
      Add(r, LogLevel::kWarn, std::string("config file key=") + k.name + " rejected reason=unset_variable");
      continue;
    }
    const char* bad = ValidateValue(k.kind, *value);
    if (bad[0] != '\0') {
      Add(r, LogLevel::kWarn, std::string("config file key=") + k.name + " rejected reason=" + bad);
      continue;
    }
    (r.config.*k.slot) = {*value, Source::kFile};
  }
  Features& want = r.config.requested;
  want.social = ReadBool(cfg, kSocialFacadeKey, true, r);
  want.presenceNames = want.social;
  want.hwdump = ReadBool(cfg, kHardwareDumpKey, false, r);
  want.presenceLocal = !ReadBool(cfg, kSendPresenceToMetaKey, true, r);
}

void Reject(LoadResult& r, const std::string& path, const std::string& reason) {
  r.fileRejected = true;
  Add(r, LogLevel::kError, "config file=" + path + " rejected reason=" + reason + " (embedded defaults stay)");
}

void ApplyFile(LoadResult& r, const std::string& text, const std::string& path) {
  if (text.size() > kMaxConfigBytes) {
    Reject(r, path, "too_large limit_bytes=" + std::to_string(kMaxConfigBytes));
    return;
  }
  // The YAML parser throws. Every throw ends here, by name; none reaches the sentinel's constructor. The reason is
  // nevr_config's token for the failure, never the parser's text, which echoes the file's own words.
  try {
    const nevr::NevrConfig cfg = nevr::NevrConfig::LoadFromString(text);
    ApplyLoaded(r, cfg, path);
    Add(r, LogLevel::kInfo, "config file=" + path + " loaded");
  } catch (const nevr::NevrConfigError& e) {
    Reject(r, path, nevr::ReasonToken(e.Kind()));
  } catch (const std::exception&) {
    Reject(r, path, "internal_error");
  }
}

bool Present(const Value& v) { return v.source != Source::kAbsent; }

// Redirect needs a target, bridge needs redirect and a socket target, login needs the bridge and
// both the socket target and the server key, social needs login. Each forced-off feature logs why.
void Derive(LoadResult& r) {
  ResolvedConfig& c = r.config;
  c.effective = c.requested;
  const auto force_off = [&r](const char* feature, const char* reason) {
    Add(r, LogLevel::kWarn, std::string("feature=") + feature + " forced off reason=" + reason);
  };
  if (c.effective.redirect && !Present(c.socketUri) && !Present(c.httpUri)) {
    c.effective.redirect = false;
    force_off("redirect", "no_target_configured");
  }
  if (c.effective.bridge && !c.effective.redirect) {
    c.effective.bridge = false;
    force_off("bridge", "redirect_not_enabled");
  }
  if (c.effective.bridge && !Present(c.socketUri)) {
    c.effective.bridge = false;
    force_off("bridge", "no_socket_uri");
  }
  if (c.effective.login && !c.effective.bridge) {
    c.effective.login = false;
    force_off("login", "bridge_not_enabled");
  }
  if (c.effective.login && !Present(c.serverKey)) {
    c.effective.login = false;
    force_off("login", "no_server_key");
  }
  // Resolved last: social needs login to be effective, so every rule above applies to it too.
  if (c.effective.social && !c.effective.login) {
    c.effective.social = false;
    force_off("social", "login_not_enabled");
  }
  // After social: the names ride in the facade's member data.
  if (c.effective.presenceNames && !c.effective.social) {
    c.effective.presenceNames = false;
    force_off("presence_names", "social_not_enabled");
  }
  if (c.effective.presenceLocal && !c.effective.social) {
    c.effective.presenceLocal = false;
    force_off("presence_local", "social_not_enabled");
  }
}

void ReportState(LoadResult& r) {
  for (const KeySpec& k : kKeys) {
    Add(r, LogLevel::kInfo,
        std::string("config key=") + k.name + " source=" + SourceName((r.config.*k.slot).source));
  }
  for (const FeatureSpec& f : kFeatures) {
    Add(r, LogLevel::kInfo,
        std::string("feature=") + f.name + " requested=" + (r.config.requested.*f.flag ? "on" : "off") +
            " effective=" + (r.config.effective.*f.flag ? "on" : "off"));
  }
}

}  // namespace

LoadResult ResolveConfig(const EmbeddedDefaults& defaults, const std::string* fileText, const std::string& filePath) {
  LoadResult r;
  ApplyEmbedded(r.config, defaults, &r.events);
  SetDefaultRequests(r.config);
  if (fileText == nullptr) {
    Add(r, LogLevel::kInfo, "config file=" + filePath + " absent, using embedded defaults");
  } else {
    ApplyFile(r, *fileText, filePath);
    if (r.fileRejected) {
      // A rejected file contributes nothing: values and features are the embedded defaults.
      r.config = ResolvedConfig();
      ApplyEmbedded(r.config, defaults, nullptr);
      SetDefaultRequests(r.config);
    }
  }
  Derive(r);
  ReportState(r);
  return r;
}

const char* LevelName(LogLevel level) {
  switch (level) {
    case LogLevel::kInfo: return "INFO";
    case LogLevel::kWarn: return "WARN";
    case LogLevel::kError: break;
  }
  return "ERROR";
}

std::string FormatDiskLogLine(LogLevel level, long long unixMs, const std::string& message) {
  nlohmann::json line;
  line["ts_unix_ms"] = unixMs;
  line["level"] = LevelName(level);
  line["tag"] = "NEVR-Sentinel";
  line["msg"] = message;
  return line.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace) + "\n";
}

namespace {
std::string InFilesDir(const std::string& filesDir, const char* name) {
  if (filesDir.empty()) return name;
  return filesDir.back() == '/' ? filesDir + name : filesDir + "/" + name;
}
}  // namespace

std::string ConfigFilePath(const std::string& filesDir) { return InFilesDir(filesDir, kConfigFileName); }

std::string LegacyConfigFilePath(const std::string& filesDir) { return InFilesDir(filesDir, kLegacyConfigFileName); }

bool FeatureEnabled(const ResolvedConfig& config, Feature feature) {
  for (const FeatureSpec& f : kFeatures) {
    if (f.feature == feature) return config.effective.*f.flag;
  }
  return false;
}

const char* FeatureName(Feature feature) {
  for (const FeatureSpec& f : kFeatures) {
    if (f.feature == feature) return f.name;
  }
  return "unknown";
}

const char* SourceName(Source source) {
  switch (source) {
    case Source::kFile: return "file";
    case Source::kEmbedded: return "embedded";
    case Source::kAbsent: break;
  }
  return "absent";
}

std::optional<std::string> ResolveQuestRedirect(const ResolvedConfig& config, const std::string& url,
                                           bool bridgeReady, unsigned bridgePort) {
  if (!config.effective.redirect) return std::nullopt;
  const auto target = [](const Value& v) -> std::optional<std::string> {
    if (v.source == Source::kAbsent || v.text.empty()) return std::nullopt;
    return v.text;
  };
  return nevr_cfg::ResolveRedirect(url, target(config.socketUri), target(config.httpUri),
                                   config.effective.bridge && bridgeReady, bridgePort);
}

}  // namespace nevr_quest
