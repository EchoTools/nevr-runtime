// nevr_config.cpp — see nevr_config.h. yaml-cpp is confined to this translation
// unit; the header exposes none of it (pImpl), so no consumer or module pulls in
// yaml-cpp. This file is compiled WITHOUT the core PCH (SKIP_PRECOMPILE_HEADERS in
// CMake): the PCH includes <windows.h> without NOMINMAX, and its min/max macros
// break yaml-cpp and <limits>.

#include "core/nevr_config.h"

#include <cctype>
#include <cstdlib>
#include <fstream>
#include <set>
#include <sstream>
#include <utility>

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>  // CHAR / VOID typedefs used by core/logging.h
#endif

#include "core/logging.h"  // Log(), FatalError()

// yaml-cpp AFTER windows.h so NOMINMAX has suppressed the min/max macros.
#include <yaml-cpp/yaml.h>

namespace nevr {

struct NevrConfig::Impl {
  YAML::Node root;  // the interpolated tree (a Map, always defined after Load)
};

namespace {

// --- environment / interpolation -------------------------------------------

// Empty is treated as unset — an empty secret is worse than a loud failure (N115).
std::optional<std::string> GetEnv(const std::string& name) {
  const char* v = std::getenv(name.c_str());
  if (v == nullptr) return std::nullopt;
  std::string s(v);
  if (s.empty()) return std::nullopt;
  return s;
}

// Resolve one `${...}` body. Forms:
//   ${VAR}        required — throws if unset
//   ${VAR:?msg}   required — throws with `msg` (or a default) if unset
//   ${VAR:-def}   optional — `def` if unset
std::string ResolveVar(const std::string& inner) {
  std::string var = inner;
  char mode = 0;  // 0 bare, '?' required-with-msg, '-' default
  std::string rest;
  const std::size_t colon = inner.find(':');
  if (colon != std::string::npos && colon + 1 < inner.size() &&
      (inner[colon + 1] == '?' || inner[colon + 1] == '-')) {
    var = inner.substr(0, colon);
    mode = inner[colon + 1];
    rest = inner.substr(colon + 2);
  }

  const std::optional<std::string> val = GetEnv(var);
  if (mode == '-') return val ? *val : rest;
  if (val) return *val;
  if (mode == '?') {
    throw NevrConfigError(rest.empty()
                              ? ("config: required environment variable " + var + " is not set")
                              : ("config: " + rest));
  }
  throw NevrConfigError("config: required environment variable " + var +
                        " is not set (referenced as ${" + var + "})");
}

// Replace every ${...} in `in`. An unterminated ${ is left literal.
std::string InterpolateString(const std::string& in) {
  std::string out;
  std::size_t i = 0;
  while (i < in.size()) {
    if (in[i] == '$' && i + 1 < in.size() && in[i + 1] == '{') {
      const std::size_t close = in.find('}', i + 2);
      if (close == std::string::npos) {
        out += in.substr(i);
        break;
      }
      out += ResolveVar(in.substr(i + 2, close - (i + 2)));
      i = close + 1;
    } else {
      out += in[i++];
    }
  }
  return out;
}

// If `n` is a scalar, return its interpolated value; otherwise nullopt.
std::optional<std::string> InterpolateScalar(const YAML::Node& n) {
  if (n.IsScalar()) return InterpolateString(n.Scalar());
  return std::nullopt;
}

// Read-only walk that interpolates every scalar and DISCARDS the result — its
// sole purpose is to make a required ${VAR:?}/${VAR} that is unset fail LOUD at
// load time (InterpolateString throws). It never mutates the tree: reassigning a
// scalar in place during map iteration corrupts later sibling map entries in
// yaml-cpp (measured — services/network/arena became unreadable after `auth`,
// the one section whose values actually changed). So interpolation is applied
// lazily at read time (GetString etc.); this walk only validates.
void ValidateInterpolation(const YAML::Node& node) {
  switch (node.Type()) {
    case YAML::NodeType::Scalar:
      (void)InterpolateString(node.Scalar());  // may throw
      break;
    case YAML::NodeType::Sequence:
      for (const auto& child : node) ValidateInterpolation(child);
      break;
    case YAML::NodeType::Map:
      for (const auto& kv : node) ValidateInterpolation(kv.second);
      break;
    default:
      break;
  }
}

// --- lookup helpers (read-only; never auto-vivify) -------------------------

YAML::Node ChildByKey(const YAML::Node& map, const std::string& key) {
  if (!map.IsMap()) return YAML::Node(YAML::NodeType::Undefined);
  for (const auto& kv : map) {
    if (kv.first.IsScalar() && kv.first.Scalar() == key) return kv.second;
  }
  return YAML::Node(YAML::NodeType::Undefined);
}

YAML::Node ResolvePath(const YAML::Node& root, const std::string& path) {
  // `cur` aliases the tree; it is advanced ONLY via reset(), never operator=.
  // `cur = ChildByKey(...)` would assign THROUGH the alias and mutate the shared
  // graph — measured: the first GetString collapsed impl_->root down to the node
  // it descended into, so every later read saw a truncated tree. reset() rebinds
  // the handle to the child without touching the node it previously referenced.
  YAML::Node cur = root;
  std::size_t start = 0;
  for (;;) {
    const std::size_t dot = path.find('.', start);
    const std::string part =
        (dot == std::string::npos) ? path.substr(start) : path.substr(start, dot - start);
    cur.reset(ChildByKey(cur, part));
    if (!cur.IsDefined() || dot == std::string::npos) return cur;
    start = dot + 1;
  }
}

std::optional<bool> ParseBool(const std::string& s) {
  std::string t;
  t.reserve(s.size());
  for (char c : s) t += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  if (t == "true" || t == "yes" || t == "on" || t == "1") return true;
  if (t == "false" || t == "no" || t == "off" || t == "0") return false;
  return std::nullopt;
}

// --- top-level validation + plugins ----------------------------------------

const std::set<std::string>& AllowedTopLevelKeys() {
  static const std::set<std::string> kKeys = {
      "version", "identity", "auth",    "services", "network", "assets",
      "arena",   "guilds",   "telemetry", "plugins", "profiles"};
  return kKeys;
}

void ValidateTopLevel(const YAML::Node& root) {
  for (const auto& kv : root) {
    const std::string key = kv.first.Scalar();
    if (AllowedTopLevelKeys().count(key) == 0 && key.rfind("x-", 0) != 0) {
      throw NevrConfigError(
          "config: unknown top-level key '" + key +
          "' (allowed: version, identity, auth, services, network, assets, arena, "
          "guilds, telemetry, plugins, profiles; use x-* for extensions)");
    }
  }
}

void FlattenArgs(const YAML::Node& node, const std::string& prefix,
                 std::map<std::string, std::string>& out) {
  for (const auto& kv : node) {
    const std::string key =
        prefix.empty() ? kv.first.Scalar() : (prefix + "." + kv.first.Scalar());
    const YAML::Node value = kv.second;
    if (value.IsMap()) {
      FlattenArgs(value, key, out);
    } else if (value.IsSequence()) {
      std::string joined;
      bool first = true;
      for (const auto& item : value) {
        if (!item.IsScalar()) continue;
        if (!first) joined += ",";
        joined += InterpolateString(item.Scalar());
        first = false;
      }
      out[key] = joined;
    } else if (value.IsScalar()) {
      out[key] = InterpolateString(value.Scalar());
    }
  }
}

void ParsePlugins(const YAML::Node& root, std::vector<PluginSpec>& out) {
  const YAML::Node plugins = ChildByKey(root, "plugins");
  if (!plugins.IsDefined() || plugins.IsNull()) return;
  if (!plugins.IsSequence()) throw NevrConfigError("config: 'plugins' must be a list");

  for (const auto& entry : plugins) {
    if (!entry.IsMap()) throw NevrConfigError("config: each plugin entry must be a mapping");

    PluginSpec spec;
    const YAML::Node nameN = ChildByKey(entry, "name");
    if (!nameN.IsScalar() || nameN.Scalar().empty()) {
      throw NevrConfigError("config: a plugin entry is missing a non-empty 'name'");
    }
    spec.name = InterpolateString(nameN.Scalar());

    const YAML::Node fileN = ChildByKey(entry, "file");
    spec.file = fileN.IsScalar() ? InterpolateString(fileN.Scalar()) : (spec.name + ".dll");

    const YAML::Node enabledN = ChildByKey(entry, "enabled");
    if (enabledN.IsScalar()) spec.enabled = ParseBool(enabledN.Scalar()).value_or(true);

    const YAML::Node requiredN = ChildByKey(entry, "required");
    if (requiredN.IsScalar()) spec.required = ParseBool(requiredN.Scalar()).value_or(false);

    const YAML::Node targetN = ChildByKey(entry, "target");
    if (targetN.IsScalar()) spec.target = InterpolateString(targetN.Scalar());

    const YAML::Node argsN = ChildByKey(entry, "args");
    if (argsN.IsDefined() && argsN.IsMap()) FlattenArgs(argsN, "", spec.args);

    out.push_back(std::move(spec));
  }
}

}  // namespace

// --- NevrConfig -------------------------------------------------------------

NevrConfig::NevrConfig() = default;
NevrConfig::~NevrConfig() = default;
NevrConfig::NevrConfig(NevrConfig&&) noexcept = default;
NevrConfig& NevrConfig::operator=(NevrConfig&&) noexcept = default;

NevrConfig NevrConfig::LoadFromString(const std::string& yaml) {
  YAML::Node raw;
  try {
    raw = YAML::Load(yaml);
  } catch (const YAML::Exception& e) {
    throw NevrConfigError(std::string("config: YAML parse error: ") + e.what());
  }

  NevrConfig cfg;
  cfg.impl_ = std::make_unique<Impl>();

  if (!raw.IsDefined() || raw.IsNull()) {
    cfg.impl_->root = YAML::Node(YAML::NodeType::Map);  // empty document is valid
    cfg.empty_ = true;
    return cfg;
  }
  if (!raw.IsMap()) throw NevrConfigError("config: the top level must be a mapping");

  ValidateTopLevel(raw);
  ValidateInterpolation(raw);  // fail loud NOW on an unset ${VAR:?}; does not mutate raw
  cfg.impl_->root = raw;       // store the parsed tree; scalars interpolate at read time
  ParsePlugins(cfg.impl_->root, cfg.plugins_);
  cfg.empty_ = false;
  return cfg;
}

NevrConfig NevrConfig::LoadFromFile(const std::string& path) {
  std::ifstream f(path, std::ios::binary);
  if (!f) throw NevrConfigError("config: cannot open " + path);
  std::stringstream ss;
  ss << f.rdbuf();
  return LoadFromString(ss.str());
}

NevrConfig NevrConfig::LoadFromFileOrFail(const std::string& path, bool is_server) {
  try {
    return LoadFromFile(path);
  } catch (const NevrConfigError& e) {
    if (is_server) {
      // In a real server the registered handler routes this to ForceFatalExit
      // (the N4 path); the unregistered fallback message-boxes + exits. Never
      // reached by the unit test, which drives the client branch only.
      FatalError(e.what(), "NEVR Config Error");
    } else {
      Log(EchoVR::LogLevel::Warning, "[NEVR.CONFIG] %s", e.what());
    }
    return NevrConfig();  // empty
  }
}

std::optional<std::string> NevrConfig::GetString(const std::string& path) const {
  if (!impl_) return std::nullopt;
  return InterpolateScalar(ResolvePath(impl_->root, path));
}

std::optional<bool> NevrConfig::GetBool(const std::string& path) const {
  const std::optional<std::string> s = GetString(path);
  if (!s) return std::nullopt;
  if (const std::optional<bool> b = ParseBool(*s)) return b;
  try {
    return YAML::Node(*s).as<bool>();
  } catch (const YAML::Exception&) {
    return std::nullopt;
  }
}

std::optional<std::int64_t> NevrConfig::GetInt(const std::string& path) const {
  const std::optional<std::string> s = GetString(path);
  if (!s) return std::nullopt;
  try {
    return YAML::Node(*s).as<std::int64_t>();
  } catch (const YAML::Exception&) {
    return std::nullopt;
  }
}

std::optional<double> NevrConfig::GetFloat(const std::string& path) const {
  const std::optional<std::string> s = GetString(path);
  if (!s) return std::nullopt;
  try {
    return YAML::Node(*s).as<double>();
  } catch (const YAML::Exception&) {
    return std::nullopt;
  }
}

std::vector<std::string> NevrConfig::GetStringList(const std::string& path) const {
  std::vector<std::string> out;
  if (!impl_) return out;
  const YAML::Node n = ResolvePath(impl_->root, path);
  if (n.IsSequence()) {
    for (const auto& child : n) {
      if (child.IsScalar()) out.push_back(InterpolateString(child.Scalar()));
    }
  } else if (n.IsScalar()) {
    out.push_back(InterpolateString(n.Scalar()));
  }
  return out;
}

}  // namespace nevr
