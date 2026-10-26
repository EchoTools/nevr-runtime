#include "quest/sentinel/quest_config.h"

#include "core/nevr_config.h"
#include "runtime/lifecycle/service_redirect.h"

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
constexpr const char* kUiEventProbeKey = "diagnostics.ui_event_probe";          // Quest only (default false)
constexpr const char* kTextureQualityKey = "graphics.texture_quality";          // Quest only (default high)

struct FeatureSpec {
  const char* name;
  Feature feature;
  bool Features::*flag;
};

constexpr std::array<FeatureSpec, 8> kFeatures = {{
    {"redirect", Feature::kRedirect, &Features::redirect},
    {"bridge", Feature::kBridge, &Features::bridge},
    {"login", Feature::kLogin, &Features::login},
    {"social", Feature::kSocial, &Features::social},
    {"hwdump", Feature::kHwDump, &Features::hwdump},
    {"presence_names", Feature::kPresenceNames, &Features::presenceNames},
    {"presence_local", Feature::kPresenceLocal, &Features::presenceLocal},
    {"ui_event_probe", Feature::kUiEventProbe, &Features::uiEventProbe},
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
  config.requested.uiEventProbe = false;
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

// How a rejected texture-quality value is named in the log: the word itself when it is short printable ASCII (a level is
// a word, not a secret), else what is wrong with it.
std::string DescribeRejectedWord(const std::string& text) {
  constexpr std::size_t kMaxNamedBytes = 32;
  if (text.empty()) return "(empty)";
  if (text.size() > kMaxNamedBytes) return "(longer than 32 bytes)";
  for (const char ch : text) {
    const auto u = static_cast<unsigned char>(ch);
    if (u <= 0x20 || u >= 0x7f) return "(not printable ASCII without spaces)";
  }
  return text;
}

// graphics.texture_quality: absent keeps the default (high); a word that is not a level keeps it too, with a warning
// naming the key and the word (#943).
void ReadTextureQuality(const nevr::NevrConfig& cfg, LoadResult& r) {
  const std::string fallback = std::string("default stays: ") + texture_quality::LevelName(texture_quality::kDefaultLevel);
  bool unsetBare = false;
  const std::optional<std::string> text = cfg.GetString(kTextureQualityKey, &unsetBare);
  if (!text) return;
  if (unsetBare) {
    Add(r, LogLevel::kWarn,
        std::string("config file key=") + kTextureQualityKey + " rejected reason=unset_variable (" + fallback + ")");
    return;
  }
  texture_quality::Level level = texture_quality::kDefaultLevel;
  if (!texture_quality::ParseLevel(*text, &level)) {
    Add(r, LogLevel::kWarn,
        std::string("config file key=") + kTextureQualityKey + " rejected reason=unknown_level value=" +
            DescribeRejectedWord(*text) + " (" + fallback +
            "; allowed: stock, low, medium, high, very_high, ultra)");
    return;
  }
  r.config.textureQuality = {level, Source::kFile};
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
  want.uiEventProbe = ReadBool(cfg, kUiEventProbeKey, false, r);
  want.presenceLocal = !ReadBool(cfg, kSendPresenceToMetaKey, true, r);
  ReadTextureQuality(cfg, r);
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
  // The probe works through the social facade's slots, so it follows social.
  if (c.effective.uiEventProbe && !c.effective.social) {
    c.effective.uiEventProbe = false;
    force_off("ui_event_probe", "social_not_enabled");
  }
}

void ReportState(LoadResult& r) {
  for (const KeySpec& k : kKeys) {
    Add(r, LogLevel::kInfo,
        std::string("config key=") + k.name + " source=" + SourceName((r.config.*k.slot).source));
  }
  Add(r, LogLevel::kInfo,
      std::string("config key=") + kTextureQualityKey + " quality=" +
          texture_quality::LevelName(r.config.textureQuality.level) +
          " source=" + TextureSourceName(r.config.textureQuality));
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

const char* TextureSourceName(const TextureChoice& choice) { return choice.source == Source::kFile ? "config" : "default"; }

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
