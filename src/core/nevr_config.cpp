// nevr_config.cpp — see nevr_config.h. yaml-cpp is confined to this translation
// unit; the header exposes none of it (pImpl), so no consumer or module pulls in
// yaml-cpp. This file is compiled WITHOUT the core PCH (SKIP_PRECOMPILE_HEADERS in
// CMake): the PCH includes <windows.h> without NOMINMAX, and its min/max macros
// break yaml-cpp and <limits>.
//
// The parser is shared with the Quest sentinel (src/quest/sentinel/quest_config.cpp), which does not build the
// PC logger: this file includes no core/logging.h and logs nothing. What the PC does on a failed load (N4) and
// its unset-variable warning live in nevr_config_policy.cpp.

// Before any header that could reach <windows.h>: its min/max macros break yaml-cpp and <limits>.
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "core/nevr_config.h"

#include <cctype>
#include <cstdlib>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <sstream>
#include <utility>

#include <yaml-cpp/yaml.h>

namespace nevr {

struct NevrConfig::Impl {
  YAML::Node root;  // the interpolated tree (a Map, always defined after Load)
};

namespace {

int SourceLine(const YAML::Mark& mark) { return mark.is_null() ? 0 : mark.line + 1; }
int SourceColumn(const YAML::Mark& mark) { return mark.is_null() ? 0 : mark.column + 1; }

// --- environment / interpolation -------------------------------------------

// Empty is treated as unset — an empty secret is worse than a loud failure (N115).
std::optional<std::string> GetEnv(const std::string& name) {
  const char* v = std::getenv(name.c_str());
  if (v == nullptr) return std::nullopt;
  std::string s(v);
  if (s.empty()) return std::nullopt;
  return s;
}

// Where unset bare ${VAR} names are collected: a pointer passed down the call, null when the caller does not
// want them. Not a thread_local: the Quest sentinel (which builds this file) allows no thread-local state of its
// own (tests/quest TestNoEmulatedTLSInHookPath).
using UnsetSink = std::set<std::string>*;

// Resolve one `${...}` body. Forms:
//   ${VAR}        optional in effect — stays literal "${VAR}" if unset (warned at load)
//   ${VAR:?msg}   required — throws with `msg` (or a default) if unset
//   ${VAR:-def}   optional — `def` if unset
std::string ResolveVar(const std::string& inner, UnsetSink unset, const YAML::Mark& mark) {
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
                              : ("config: " + rest),
                          NevrConfigErrorKind::kUnsetRequiredVariable, SourceLine(mark), SourceColumn(mark));
  }
  // Bare ${VAR}, unset: the text stays as written. Failing here would cost a client
  // its whole config (every plugin) over one value; secrets use ${VAR:?msg}, which
  // still throws. The name is collected for the caller (UnsetBareVars()), which reports it.
  if (unset != nullptr) unset->insert(var);
  return "${" + inner + "}";
}

// Replace every ${...} in `in`. An unterminated ${ is left literal. The three-char
// sequence $${ becomes a literal ${ and nothing after it is looked up, so it is the
// way to write ${ that isn't a variable (e.g. in a plugin's args, whose value would
// otherwise be read as a variable reference). No other `$` is special: $$ not followed by
// { stays $$, and $$${X} yields $${X}. A literal `$` immediately followed by a
// variable's value is written with a default: ${NO_SUCH_VAR:-$}${X}.
std::string InterpolateString(const std::string& in, UnsetSink unset = nullptr,
                              const YAML::Mark& mark = YAML::Mark::null_mark()) {
  std::string out;
  std::size_t i = 0;
  while (i < in.size()) {
    if (in[i] == '$' && i + 2 < in.size() && in[i + 1] == '$' && in[i + 2] == '{') {
      out += "${";
      i += 3;
    } else if (in[i] == '$' && i + 1 < in.size() && in[i + 1] == '{') {
      const std::size_t close = in.find('}', i + 2);
      if (close == std::string::npos) {
        out += in.substr(i);
        break;
      }
      out += ResolveVar(in.substr(i + 2, close - (i + 2)), unset, mark);
      i = close + 1;
    } else {
      out += in[i++];
    }
  }
  return out;
}

// If `n` is a scalar, return its interpolated value; otherwise nullopt.
std::optional<std::string> InterpolateScalar(const YAML::Node& n, UnsetSink unset = nullptr) {
  if (n.IsScalar()) return InterpolateString(n.Scalar(), unset, n.Mark());
  return std::nullopt;
}

// The bound on a walk over the file's tree (kMaxConfigDepth, kMaxConfigNodes in nevr_config.h): one per load,
// shared by every walk, so a file cannot spend the node budget twice. Passed down the call, not a global.
struct WalkBudget {
  std::size_t visited = 0;
};

// Count one visit of a node at `depth`; throws when the walk has gone too deep or too far. An alias makes the
// tree a cycle or a DAG that expands to exponentially many visits, so both are checked on every visit.
void EnterNode(WalkBudget& budget, std::size_t depth, const YAML::Mark& mark) {
  if (depth > kMaxConfigDepth) {
    throw NevrConfigError("config: nesting is deeper than " + std::to_string(kMaxConfigDepth) + " levels",
                          NevrConfigErrorKind::kNestingTooDeep, SourceLine(mark), SourceColumn(mark));
  }
  if (++budget.visited > kMaxConfigNodes) {
    throw NevrConfigError("config: more than " + std::to_string(kMaxConfigNodes) + " nodes",
                          NevrConfigErrorKind::kTooManyNodes, SourceLine(mark), SourceColumn(mark));
  }
}

// Read-only walk that interpolates every scalar and DISCARDS the result — its
// sole purpose is to make a required ${VAR:?} that is unset fail LOUD at
// load time (InterpolateString throws) and to collect unset bare ${VAR} names. It never mutates the tree: reassigning a
// scalar in place during map iteration corrupts later sibling map entries in
// yaml-cpp (measured — services/network/arena became unreadable after `auth`,
// the one section whose values actually changed). So interpolation is applied
// lazily at read time (GetString etc.); this walk only validates. Bounded by `budget`.
// It also counts, into `duplicates`, each key that repeats within its mapping (#583): it already visits every
// mapping, so the count costs no second walk and no node budget.
void ValidateInterpolation(const YAML::Node& node, UnsetSink unset, WalkBudget& budget, std::size_t depth,
                           std::size_t& duplicates) {
  EnterNode(budget, depth, node.Mark());
  switch (node.Type()) {
    case YAML::NodeType::Scalar:
      (void)InterpolateString(node.Scalar(), unset, node.Mark());  // may throw
      break;
    case YAML::NodeType::Sequence:
      for (const auto& child : node) ValidateInterpolation(child, unset, budget, depth + 1, duplicates);
      break;
    case YAML::NodeType::Map: {
      std::set<std::string> seen;
      for (const auto& kv : node) {
        if (kv.first.IsScalar() && !seen.insert(kv.first.Scalar()).second) ++duplicates;
        ValidateInterpolation(kv.second, unset, budget, depth + 1, duplicates);
      }
      break;
    }
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

// --- top-level validation + plugins ----------------------------------------

const std::set<std::string>& AllowedTopLevelKeys() {
  static const std::set<std::string> kKeys = {
      "version", "identity", "auth",    "services", "network", "assets",
      "arena",   "guilds",   "telemetry", "plugins", "profiles", "social",
      "diagnostics", "graphics"};
  return kKeys;
}

void ValidateTopLevel(const YAML::Node& root) {
  for (const auto& kv : root) {
    const std::string key = kv.first.Scalar();
    if (AllowedTopLevelKeys().count(key) == 0 && key.rfind("x-", 0) != 0) {
      throw NevrConfigError(
          "config: unknown top-level key '" + key +
          "' (allowed: version, identity, auth, services, network, assets, arena, "
          "guilds, telemetry, plugins, profiles, social, diagnostics, graphics; use x-* for extensions)",
          NevrConfigErrorKind::kUnknownTopLevelKey, SourceLine(kv.first.Mark()), SourceColumn(kv.first.Mark()));
    }
  }
}

// The keys each known section holds, for every reader of either build (#583). The PC reads a key through
// service_map.cpp (FlatKeyToYamlPath) or a direct getter; the Quest reads its keys through quest_config.cpp.
// A key missing here is ignored by the reader, so it is counted as unknown. Keep it in step with those readers:
// test_service_map (MigratedYamlPaths) and test_nevr_config (the Quest list) fail when a read key is missing.
const std::map<std::string, std::set<std::string>>& KnownSections() {
  static const std::map<std::string, std::set<std::string>> kSections = {
      {"identity", {"discord_id"}},
      {"auth", {"http_uri", "http_key", "server_key", "password"}},
      {"services",
       {"login", "serverdb", "matchmaking", "api", "api_legacy", "config", "transaction", "graph", "graph_service",
        "socket_uri", "allow_offline_server", "serverdb_uri"}},
      {"network",
       {"external_ip", "internal_ip", "upnp", "upnp_port", "empty_server_ttl_seconds", "tick_rate_hz", "session_router",
        "regions", "invite_codes"}},
      {"assets", {"cdn_url"}},
      {"arena", {"round_time", "celebration_time", "mercy_score"}},
      {"telemetry", {"uri", "token"}},
      {"social", {"facade", "send_presence_to_meta"}},
      {"diagnostics", {"hardware_dump", "ui_event_probe", "frame_stats"}},
      {"graphics", {"texture_quality"}},
  };
  return kSections;
}

// Keys under a known section that no reader reads, counted once per key. An `x-` key is an extension and is not
// counted, as at the top level. A section that is not a mapping has no keys to count.
std::size_t CountUnknownNestedKeys(const YAML::Node& root) {
  std::size_t unknown = 0;
  for (const auto& section : KnownSections()) {
    const YAML::Node node = ChildByKey(root, section.first);
    if (!node.IsMap()) continue;
    for (const auto& kv : node) {
      if (!kv.first.IsScalar()) {
        ++unknown;
        continue;
      }
      const std::string key = kv.first.Scalar();
      if (key.rfind("x-", 0) == 0 || section.second.count(key) != 0) continue;
      ++unknown;
    }
  }
  return unknown;
}

// Bounded by `budget` like ValidateInterpolation; `depth` is the depth of `node` in the file's tree.
void FlattenArgs(const YAML::Node& node, const std::string& prefix, std::map<std::string, std::string>& out,
                 WalkBudget& budget, std::size_t depth) {
  EnterNode(budget, depth, node.Mark());
  for (const auto& kv : node) {
    const std::string key =
        prefix.empty() ? kv.first.Scalar() : (prefix + "." + kv.first.Scalar());
    const YAML::Node value = kv.second;
    if (value.IsMap()) {
      FlattenArgs(value, key, out, budget, depth + 1);
    } else if (value.IsSequence()) {
      std::string joined;
      bool first = true;
      for (const auto& item : value) {
        if (!item.IsScalar()) continue;
        if (!first) joined += ",";
        joined += InterpolateString(item.Scalar(), nullptr, item.Mark());
        first = false;
      }
      out[key] = joined;
    } else if (value.IsScalar()) {
      out[key] = InterpolateString(value.Scalar(), nullptr, value.Mark());
    }
  }
}

void ParsePlugins(const YAML::Node& root, std::vector<PluginSpec>& out, WalkBudget& budget) {
  const YAML::Node plugins = ChildByKey(root, "plugins");
  if (!plugins.IsDefined() || plugins.IsNull()) return;
  if (!plugins.IsSequence()) {
    throw NevrConfigError("config: 'plugins' must be a list", NevrConfigErrorKind::kBadPlugins,
                          SourceLine(plugins.Mark()), SourceColumn(plugins.Mark()));
  }

  for (const auto& entry : plugins) {
    if (!entry.IsMap()) {
      throw NevrConfigError("config: each plugin entry must be a mapping", NevrConfigErrorKind::kBadPlugins,
                            SourceLine(entry.Mark()), SourceColumn(entry.Mark()));
    }

    PluginSpec spec;
    const YAML::Node nameN = ChildByKey(entry, "name");
    if (!nameN.IsScalar() || nameN.Scalar().empty()) {
      throw NevrConfigError("config: a plugin entry is missing a non-empty 'name'", NevrConfigErrorKind::kBadPlugins,
                            SourceLine(entry.Mark()), SourceColumn(entry.Mark()));
    }
    spec.name = InterpolateString(nameN.Scalar(), nullptr, nameN.Mark());

    const YAML::Node fileN = ChildByKey(entry, "file");
    spec.file = fileN.IsScalar() ? InterpolateString(fileN.Scalar(), nullptr, fileN.Mark()) : (spec.name + ".dll");

    const YAML::Node enabledN = ChildByKey(entry, "enabled");
    if (enabledN.IsScalar()) spec.enabled = ParseBool(enabledN.Scalar()).value_or(true);

    const YAML::Node requiredN = ChildByKey(entry, "required");
    if (requiredN.IsScalar()) spec.required = ParseBool(requiredN.Scalar()).value_or(false);

    const YAML::Node earlyN = ChildByKey(entry, "early");
    if (earlyN.IsScalar()) spec.early = ParseBool(earlyN.Scalar()).value_or(false);

    const YAML::Node targetN = ChildByKey(entry, "target");
    if (targetN.IsScalar()) spec.target = InterpolateString(targetN.Scalar(), nullptr, targetN.Mark());

    const YAML::Node argsN = ChildByKey(entry, "args");
    // root map 0, `plugins` list 1, the entry 2, `args` 3.
    if (argsN.IsDefined() && argsN.IsMap()) FlattenArgs(argsN, "", spec.args, budget, 3);

    out.push_back(std::move(spec));
  }
}

}  // namespace

const char* ReasonToken(NevrConfigErrorKind kind) {
  switch (kind) {
    case NevrConfigErrorKind::kUnreadable: return "unreadable";
    case NevrConfigErrorKind::kYamlSyntax: return "yaml_syntax";
    case NevrConfigErrorKind::kNotAMapping: return "not_a_mapping";
    case NevrConfigErrorKind::kUnknownTopLevelKey: return "unknown_top_level_key";
    case NevrConfigErrorKind::kBadPlugins: return "bad_plugins";
    case NevrConfigErrorKind::kUnsetRequiredVariable: return "unset_required_variable";
    case NevrConfigErrorKind::kNestingTooDeep: return "nesting_too_deep";
    case NevrConfigErrorKind::kTooManyNodes: return "too_many_nodes";
    case NevrConfigErrorKind::kOther: break;
  }
  return "invalid_config";
}

std::optional<bool> ParseBool(const std::string& s) {
  std::string t;
  t.reserve(s.size());
  for (char c : s) t += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  if (t == "true" || t == "yes" || t == "on" || t == "1") return true;
  if (t == "false" || t == "no" || t == "off" || t == "0") return false;
  return std::nullopt;
}

std::vector<std::string> KnownConfigKeyPaths() {
  std::vector<std::string> paths;
  for (const auto& section : KnownSections()) {
    for (const std::string& key : section.second) paths.push_back(section.first + "." + key);
  }
  return paths;  // std::map and std::set keep both levels sorted
}

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
    throw NevrConfigError(std::string("config: YAML parse error: ") + e.what(), NevrConfigErrorKind::kYamlSyntax,
                          SourceLine(e.mark), SourceColumn(e.mark));
  }

  NevrConfig cfg;
  cfg.impl_ = std::make_unique<Impl>();

  if (!raw.IsDefined() || raw.IsNull()) {
    cfg.impl_->root = YAML::Node(YAML::NodeType::Map);  // empty document is valid
    cfg.empty_ = true;
    return cfg;
  }
  if (!raw.IsMap()) {
    throw NevrConfigError("config: the top level must be a mapping", NevrConfigErrorKind::kNotAMapping,
                          SourceLine(raw.Mark()), SourceColumn(raw.Mark()));
  }

  ValidateTopLevel(raw);
  std::set<std::string> unsetBare;
  WalkBudget budget;
  // fail loud NOW on an unset ${VAR:?}; does not mutate raw
  ValidateInterpolation(raw, &unsetBare, budget, 0, cfg.duplicateKeys_);
  cfg.unknownKeys_ = CountUnknownNestedKeys(raw);
  cfg.unsetBareVars_.assign(unsetBare.begin(), unsetBare.end());
  cfg.impl_->root = raw;       // store the parsed tree; scalars interpolate at read time
  ParsePlugins(cfg.impl_->root, cfg.plugins_, budget);
  cfg.empty_ = false;
  return cfg;
}

NevrConfig NevrConfig::LoadFromFile(const std::string& path) {
  std::ifstream f(path, std::ios::binary);
  if (!f) throw NevrConfigError("config: cannot open " + path, NevrConfigErrorKind::kUnreadable);
  std::stringstream ss;
  ss << f.rdbuf();
  return LoadFromString(ss.str());
}

std::optional<std::string> NevrConfig::GetString(const std::string& path, bool* hadUnsetBare) const {
  if (!impl_) return std::nullopt;
  if (hadUnsetBare == nullptr) return InterpolateScalar(ResolvePath(impl_->root, path));
  std::set<std::string> unset;
  std::optional<std::string> value = InterpolateScalar(ResolvePath(impl_->root, path), &unset);
  *hadUnsetBare = !unset.empty();
  return value;
}

NodeKind NevrConfig::Kind(const std::string& path) const {
  if (!impl_) return NodeKind::kAbsent;
  const YAML::Node n = ResolvePath(impl_->root, path);
  if (!n.IsDefined()) return NodeKind::kAbsent;
  switch (n.Type()) {
    case YAML::NodeType::Scalar: return NodeKind::kScalar;
    case YAML::NodeType::Sequence: return NodeKind::kSequence;
    case YAML::NodeType::Map: return NodeKind::kMapping;
    case YAML::NodeType::Null: return NodeKind::kNull;
    case YAML::NodeType::Undefined: return NodeKind::kAbsent;
  }
  return NodeKind::kAbsent;
}

std::vector<std::string> NevrConfig::MappingKeys(const std::string& path) const {
  std::vector<std::string> keys;
  if (!impl_) return keys;
  const YAML::Node n = ResolvePath(impl_->root, path);
  if (!n.IsDefined() || !n.IsMap()) return keys;
  for (const auto& entry : n) {
    if (entry.first.IsScalar()) keys.push_back(entry.first.Scalar());
  }
  return keys;
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
