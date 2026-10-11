#include "quest/sentinel/quest_config.h"

#include "runtime/lifecycle/service_redirect.h"

#include <nlohmann/json.hpp>

#include <array>
#include <cstring>
#include <map>
#include <set>
#include <string_view>
#include <utility>

namespace nevr_quest {
namespace {

constexpr std::size_t kMaxUriBytes = 2048;
constexpr std::size_t kMaxKeyBytes = 512;
// A file of thousands of bad keys must not become thousands of log writes at startup.
constexpr std::size_t kMaxFileWarnings = 32;

enum class Kind { kSocketUri, kHttpUri, kSecret };

struct KeySpec {
  const char* name;
  Kind kind;
  Value ResolvedConfig::*slot;
};

constexpr std::array<KeySpec, 4> kKeys = {{
    {"nevr_socket_uri", Kind::kSocketUri, &ResolvedConfig::socketUri},
    {"nevr_http_uri", Kind::kHttpUri, &ResolvedConfig::httpUri},
    {"nevr_http_key", Kind::kSecret, &ResolvedConfig::httpKey},
    {"nevr_server_key", Kind::kSecret, &ResolvedConfig::serverKey},
}};

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
    {"obb_skip", Feature::kObbSkip, &Features::obbSkip},
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

// A name taken from the file is logged only when it is one of ours. Anything else (a typo, a value
// pasted as a key, a token of any shape) is counted, never echoed.
const char* KnownName(const std::string& name) {
  if (name == "features") return "features";
  for (const KeySpec& k : kKeys) {
    if (name == k.name) return k.name;
  }
  for (const FeatureSpec& f : kFeatures) {
    if (name == f.name) return f.name;
  }
  return "<unknown>";
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
  // Default-on features: a name this build does not know is reported once and ignored.
  const std::string_view list = d.features == nullptr ? "" : d.features;
  std::size_t start = 0;
  while (start < list.size()) {
    std::size_t end = list.find(',', start);
    if (end == std::string_view::npos) end = list.size();
    const std::string_view name = list.substr(start, end - start);
    start = end + 1;
    if (name.empty()) continue;
    const FeatureSpec* spec = nullptr;
    for (const FeatureSpec& f : kFeatures) {
      if (name == f.name) spec = &f;
    }
    if (spec != nullptr) {
      config.requested.*spec->flag = true;
    } else if (events != nullptr) {
      events->push_back({LogLevel::kWarn, "embedded default feature ignored reason=unknown_name"});
    }
  }
}

struct WarnBudget {
  std::size_t emitted = 0;
  std::size_t suppressed = 0;
};

void ApplyFileImpl(LoadResult& r, const std::string& text, WarnBudget& budget) {
  const auto warn = [&r, &budget](std::string message) {
    if (budget.emitted < kMaxFileWarnings) {
      ++budget.emitted;
      Add(r, LogLevel::kWarn, std::move(message));
    } else {
      ++budget.suppressed;
    }
  };
  if (text.size() > kMaxConfigBytes) {
    r.fileRejected = true;
    Add(r, LogLevel::kError,
        "config file rejected reason=too_large limit_bytes=" + std::to_string(kMaxConfigBytes));
    return;
  }
  // The parser keeps the last of two equal keys; the callback sees every key so duplicates are logged.
  std::vector<std::set<std::string>> seen;
  std::map<std::string, std::size_t> duplicates;
  std::size_t unknownKeys = 0;
  std::size_t unknownFeatures = 0;
  const nlohmann::json::parser_callback_t onParse = [&seen, &duplicates](int, nlohmann::json::parse_event_t event,
                                                                         nlohmann::json& parsed) {
    if (event == nlohmann::json::parse_event_t::object_start) {
      seen.emplace_back();
    } else if (event == nlohmann::json::parse_event_t::object_end) {
      if (!seen.empty()) seen.pop_back();
    } else if (event == nlohmann::json::parse_event_t::key && !seen.empty() && parsed.is_string()) {
      if (!seen.back().insert(parsed.get<std::string>()).second) ++duplicates[parsed.get<std::string>()];
    }
    return true;
  };
  const nlohmann::json doc = nlohmann::json::parse(text, onParse, /*allow_exceptions=*/false);
  for (const auto& entry : duplicates) {
    warn(std::string("config file duplicate key=") + KnownName(entry.first) + " extra=" + std::to_string(entry.second) +
         " last value wins");
  }
  if (doc.is_discarded()) {
    r.fileRejected = true;
    Add(r, LogLevel::kError, "config file rejected reason=malformed_json");
    return;
  }
  if (!doc.is_object()) {
    r.fileRejected = true;
    Add(r, LogLevel::kError, "config file rejected reason=top_level_not_object");
    return;
  }

  for (const auto& item : doc.items()) {
    const std::string& name = item.key();
    const nlohmann::json& value = item.value();
    if (name == "features") continue;
    const KeySpec* spec = nullptr;
    for (const KeySpec& k : kKeys) {
      if (name == k.name) spec = &k;
    }
    if (spec == nullptr) {
      warn("config file unknown key #" + std::to_string(++unknownKeys) + " ignored");
      continue;
    }
    if (!value.is_string()) {
      warn(std::string("config file key=") + spec->name + " rejected reason=not_a_string");
      continue;
    }
    const std::string& text_value = value.get_ref<const std::string&>();
    const char* bad = ValidateValue(spec->kind, text_value);
    if (bad[0] != '\0') {
      warn(std::string("config file key=") + spec->name + " rejected reason=" + bad);
      continue;
    }
    (r.config.*spec->slot) = {text_value, Source::kFile};
  }

  const auto features = doc.find("features");
  if (features == doc.end()) return;
  if (!features->is_object()) {
    warn("config file features rejected reason=not_an_object (all features stay off)");
    return;
  }
  for (const auto& item : features->items()) {
    const FeatureSpec* spec = nullptr;
    for (const FeatureSpec& f : kFeatures) {
      if (item.key() == f.name) spec = &f;
    }
    if (spec == nullptr) {
      warn("config file unknown feature #" + std::to_string(++unknownFeatures) + " ignored");
      continue;
    }
    if (!item.value().is_boolean()) {
      warn(
          std::string("config file feature=") + spec->name + " rejected reason=not_a_boolean (stays off)");
      continue;
    }
    r.config.requested.*spec->flag = item.value().get<bool>();
  }
}


void ApplyFile(LoadResult& r, const std::string& text) {
  WarnBudget budget;
  ApplyFileImpl(r, text, budget);
  if (budget.suppressed != 0) {
    Add(r, LogLevel::kWarn,
        "config file further warnings suppressed count=" + std::to_string(budget.suppressed));
  }
}

bool Present(const Value& v) { return v.source != Source::kAbsent; }

// Redirect needs a target, bridge needs redirect and a socket target, login needs the bridge and
// both the socket target and the server key, social needs login, the UI event probe needs social. Each
// forced-off feature logs why.
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
  for (const FeatureSpec& f : kFeatures) {
    Add(r, LogLevel::kInfo,
        std::string("feature=") + f.name + " requested=" + (r.config.requested.*f.flag ? "on" : "off") +
            " effective=" + (r.config.effective.*f.flag ? "on" : "off"));
  }
}

}  // namespace

LoadResult ResolveConfig(const EmbeddedDefaults& defaults, const std::string* fileText) {
  LoadResult r;
  ApplyEmbedded(r.config, defaults, &r.events);
  if (fileText == nullptr) {
    Add(r, LogLevel::kInfo, std::string("config file ") + kConfigFileName + " absent, using embedded defaults");
  } else {
    ApplyFile(r, *fileText);
    if (r.fileRejected) {
      // A rejected file contributes nothing: values and features are the embedded defaults.
      r.config = ResolvedConfig();
      ApplyEmbedded(r.config, defaults, nullptr);
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

std::string ConfigFilePath(const std::string& filesDir) {
  if (filesDir.empty()) return kConfigFileName;
  return filesDir.back() == '/' ? filesDir + kConfigFileName : filesDir + "/" + kConfigFileName;
}

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
