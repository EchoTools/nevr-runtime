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
constexpr const char* kUiEventProbeKey = "diagnostics.ui_event_probe";          // Quest only (default on with social, #318)
constexpr const char* kPnsradProviderKey = "diagnostics.pnsrad_provider";        // Quest only (default false, #1220)
constexpr const char* kTextureQualityKey = "graphics.texture_quality";          // Quest only (default stock)
constexpr const char* kTextureQualitySection = "graphics";
constexpr const char* kTextureQualityName = "texture_quality";  // the one key `graphics` has
constexpr std::size_t kMaxUnknownKeyWarnings = 8;  // a file full of unknown keys must not flood the log

struct FeatureSpec {
  const char* name;
  Feature feature;
  bool Features::*flag;
};

constexpr std::array<FeatureSpec, 9> kFeatures = {{
    {"redirect", Feature::kRedirect, &Features::redirect},
    {"bridge", Feature::kBridge, &Features::bridge},
    {"login", Feature::kLogin, &Features::login},
    {"social", Feature::kSocial, &Features::social},
    {"hwdump", Feature::kHwDump, &Features::hwdump},
    {"presence_names", Feature::kPresenceNames, &Features::presenceNames},
    {"presence_local", Feature::kPresenceLocal, &Features::presenceLocal},
    {"ui_event_probe", Feature::kUiEventProbe, &Features::uiEventProbe},
    {"pnsrad_provider", Feature::kPnsradProvider, &Features::pnsradProvider},
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
  config.requested.uiEventProbe = true;
  config.requested.pnsradProvider = false;
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

// What a mistake in graphics.texture_quality gives: `stock`, never the default (texture_quality.h kFallbackLevel), and one
// Warning that names the key. `why` is a token and carries none of the file's words.
void FallBackToStock(LoadResult& r, const std::string& why) {
  r.config.textureQuality = {texture_quality::kFallbackLevel, Source::kAbsent};
  Add(r, LogLevel::kWarn,
      std::string("config file key=") + kTextureQualityKey + " rejected reason=" + why + " (falls back to " +
          texture_quality::LevelName(texture_quality::kFallbackLevel) + "; allowed: stock, very_low, low, medium, high, very_high, ultra)");
}

// Any key under `graphics` other than texture_quality is a mistake the file's author wants to hear about: a misspelling
// (`texture_qualty`) or a different case (`Texture_Quality`) would otherwise give the default without a word, and when the
// default is raised that is a silent change of level. The level itself is not touched (the key is absent, so it is the
// default). The name is the file's own word, so it is named only when it is short printable ASCII (DescribeRejectedWord).
void WarnUnknownGraphicsKeys(const nevr::NevrConfig& cfg, LoadResult& r) {
  if (cfg.Kind(kTextureQualitySection) != nevr::NodeKind::kMapping) return;
  std::size_t unknown = 0;
  for (const std::string& key : cfg.MappingKeys(kTextureQualitySection)) {
    if (key == kTextureQualityName) continue;
    if (++unknown > kMaxUnknownKeyWarnings) continue;
    Add(r, LogLevel::kWarn,
        std::string("config file key=") + kTextureQualitySection + " rejected reason=unknown_key name=" +
            DescribeRejectedWord(key) + " (ignored; the only key under " + kTextureQualitySection + " is " +
            kTextureQualityName + ")");
  }
  if (unknown > kMaxUnknownKeyWarnings) {
    Add(r, LogLevel::kWarn,
        std::string("config file key=") + kTextureQualitySection + " rejected reason=unknown_key more=" +
            std::to_string(unknown - kMaxUnknownKeyWarnings) + " (ignored)");
  }
}

// graphics.texture_quality (#943). Absent: the default (texture_quality.h kDefaultLevel). One of the seven words: that level.
// Anything else is a mistake and falls back to `stock`, never to the default, with a warning naming the key: a word that is
// not a level (wrong case, a number, empty), a value that is not a word (a mapping, a list, a key with no value), a bare
// ${VAR} that is not set, or a `graphics` section that is not a mapping.
void ReadTextureQuality(const nevr::NevrConfig& cfg, LoadResult& r) {
  WarnUnknownGraphicsKeys(cfg, r);
  bool unsetBare = false;
  const std::optional<std::string> text = cfg.GetString(kTextureQualityKey, &unsetBare);
  if (!text) {
    // The getter gives nullopt for an absent key and for a node that is not a scalar alike; the kind tells them apart.
    if (cfg.Kind(kTextureQualityKey) != nevr::NodeKind::kAbsent) {
      FallBackToStock(r, "not_a_word");  // a mapping, a list, or a key with no value
    } else {
      const nevr::NodeKind section = cfg.Kind(kTextureQualitySection);
      // `graphics:` alone (no value) is no mistake; `graphics: ultra` and `graphics: [ultra]` are.
      if (section == nevr::NodeKind::kScalar || section == nevr::NodeKind::kSequence) {
        FallBackToStock(r, "graphics_not_a_mapping");
      }
    }
    return;
  }
  if (unsetBare) {
    FallBackToStock(r, "unset_variable");
    return;
  }
  texture_quality::Level level = texture_quality::kFallbackLevel;
  if (!texture_quality::ParseLevel(*text, &level)) {
    FallBackToStock(r, "unknown_level value=" + DescribeRejectedWord(*text));
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
  if (cfg.DuplicateKeyCount() != 0 || cfg.UnknownKeyCount() != 0) {
    // Counts only (#583): the first value of a duplicate is the one read, and an unknown key is ignored.
    Add(r, LogLevel::kWarn,
        "config file=" + path + " duplicates=" + std::to_string(cfg.DuplicateKeyCount()) +
            " unknown_nested=" + std::to_string(cfg.UnknownKeyCount()) +
            " (a duplicate key's first value is read; an unknown key is ignored; no names or values are logged)");
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
  // Defaults to the social facade's state (on unless social.facade is false), like presence_names: a facade-off file
  // gets no forced-off warning for a key it never asked for.
  want.uiEventProbe = ReadBool(cfg, kUiEventProbeKey, want.social, r);
  want.presenceLocal = !ReadBool(cfg, kSendPresenceToMetaKey, true, r);
  want.pnsradProvider = ReadBool(cfg, kPnsradProviderKey, false, r);
  ReadTextureQuality(cfg, r);
  r.config.plugins = cfg.Plugins();
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
  // The rad provider sends no login of its own: the login goes through NEVR's (#1220), so it follows login.
  if (c.effective.pnsradProvider && !c.effective.login) {
    c.effective.pnsradProvider = false;
    force_off("pnsrad_provider", "login_not_enabled");
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
      // The file might have said anything about the texture quality, so the answer is the mistake's, not the default's.
      FallBackToStock(r, "file_rejected");
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
