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
#include "core/nevr_config_testing.h"

#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <sstream>
#include <system_error>
#include <utility>
#include <vector>

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
      "diagnostics", "graphics", "combat"};
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
       {"external_ip", "internal_ip", "upnp", "upnp_port", "empty_server_ttl_seconds", "tick_rate_hz", "bots_per_session",
        "max_team_size",
        "session_router", "regions", "invite_codes"}},
      {"assets", {"cdn_url"}},
      {"arena", {"round_time", "celebration_time", "mercy_score"}},
      {"telemetry", {"uri", "token"}},
      {"social", {"facade", "send_presence_to_meta"}},
      {"diagnostics", {"hardware_dump", "pnsrad_provider", "ui_event_probe", "clock_levels_override", "frame_stats"}},
      {"graphics", {"texture_quality", "hand_flip"}},
      {"combat", {"force"}},
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
  NevrConfig cfg = LoadFromString(ss.str());
  cfg.boundFile_ = path;
  return cfg;
}

// --- NevrConfig::Set: the writer ----------------------------------------------------------------------------------------
//
// Format-preserving edit through the parser's source positions, as ruamel.yaml's round-trip mode (https://yaml.readthedocs.io/)
// and toml_edit (https://docs.rs/toml_edit/) do it: yaml-cpp parses the text, `YAML::Node::Mark()` says where the key's value
// (or the section's block) is, and bytes are spliced at exactly that span. The document is never re-emitted (yaml-cpp's emitter
// drops comments). The result is parsed again and must read back as the value, or nothing is written.

namespace {

namespace fs = std::filesystem;

struct Splice {
  bool ok = false;
  bool unchanged = false;
  const char* reason = "";
};

Splice Refused(const char* reason) {
  Splice s;
  s.reason = reason;
  return s;
}

// Offsets of each line's first byte (a line ends at '\n'; a '\r' before it belongs to the line's ending).
std::vector<std::size_t> LineStarts(const std::string& text) {
  std::vector<std::size_t> starts{0};
  for (std::size_t i = 0; i < text.size(); ++i) {
    if (text[i] == '\n') starts.push_back(i + 1);
  }
  return starts;
}

// The byte offset a yaml-cpp mark names (0-based line and column, in bytes), or false when it lies outside the text.
bool OffsetOf(const std::vector<std::size_t>& starts, const std::string& text, const YAML::Mark& mark, std::size_t* offset) {
  if (mark.is_null() || mark.line < 0 || mark.column < 0 || static_cast<std::size_t>(mark.line) >= starts.size()) return false;
  const std::size_t at = starts[static_cast<std::size_t>(mark.line)] + static_cast<std::size_t>(mark.column);
  if (at > text.size()) return false;
  *offset = at;
  return true;
}

// [begin, end) of line `index` without its ending.
void LineSpan(const std::vector<std::size_t>& starts, const std::string& text, std::size_t index, std::size_t* begin, std::size_t* end) {
  *begin = starts[index];
  std::size_t e = index + 1 < starts.size() ? starts[index + 1] - 1 : text.size();  // drop '\n'
  if (e > *begin && text[e - 1] == '\r') --e;
  *end = e;
}

// True when [from, to) holds only spaces/tabs, or those followed by a `#` comment.
bool OnlySpaceOrComment(const std::string& text, std::size_t from, std::size_t to) {
  std::size_t i = from;
  while (i < to && (text[i] == ' ' || text[i] == '\t')) ++i;
  return i == to || text[i] == '#';
}

const char* BoolText(bool value) { return value ? "true" : "false"; }

}  // namespace

// What Set writes: a boolean (written as true/false) or a plain word.
struct NevrConfig::ScalarEdit {
  std::string text;
  bool isBool = false;
  bool boolValue = false;
};

namespace {

using ScalarEdit = NevrConfig::ScalarEdit;

// A word Set may write without quotes: letters, digits, `_`, `.` and `-`, starting with a letter, digit or `_`, and not a word
// YAML 1.1 reads as a boolean or a null, so a getter reads back the string that was written.
bool IsPlainWord(const std::string& word) {
  if (word.empty() || word.size() > 64) return false;
  // Starts with a letter or `_`: a word that starts with a digit reads as a number in YAML ("123", "1.5", "1e3", "0x1f", "1_000"),
  // and Set must write what GetString reads back.
  const char first = word[0];
  if (!((first >= 'a' && first <= 'z') || (first >= 'A' && first <= 'Z') || first == '_')) return false;
  for (std::size_t i = 0; i < word.size(); ++i) {
    const char c = word[i];
    const bool alnum = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
    if (!alnum && (i == 0 || (c != '.' && c != '-'))) return false;
  }
  std::string lower;
  for (const char c : word) lower += static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
  for (const char* reserved : {"true", "false", "yes", "no", "on", "off", "y", "n", "null"}) {
    if (lower == reserved) return false;
  }
  return true;
}

bool ScalarMatches(const YAML::Node& node, const ScalarEdit& edit) {
  if (!node.IsDefined() || !node.IsScalar()) return false;
  if (!edit.isBool) return node.Scalar() == edit.text;
  bool got = false;
  return YAML::convert<bool>::decode(node, got) && got == edit.boolValue;
}

bool ReadsBack(const std::string& text, const std::string& section, const std::string& key, const ScalarEdit& edit) {
  try {
    const std::vector<YAML::Node> docs = YAML::LoadAll(text);
    if (docs.size() != 1 || !docs[0].IsMap()) return false;
    const YAML::Node& rootNode = docs[0];  // const: reading must not create the key
    const YAML::Node sectionNode = rootNode[section];
    if (!sectionNode.IsDefined() || !sectionNode.IsMap()) return false;
    const YAML::Node keyNode = static_cast<const YAML::Node&>(sectionNode)[key];
    return ScalarMatches(keyNode, edit);
  } catch (const YAML::Exception&) {
    return false;
  }
}

// Where `text` ends its last block line of `section` (a mapping whose key is at column `keyColumn` on line `keyLine`): the
// offset just past that line's ending (or text.size()), so an insert lands after the last content line and before any
// blank or comment lines that separate this block from the next.
std::size_t BlockEnd(const std::vector<std::size_t>& starts, const std::string& text, std::size_t keyLine, std::size_t keyColumn) {
  std::size_t last = keyLine;
  for (std::size_t line = keyLine + 1; line < starts.size(); ++line) {
    std::size_t b = 0, e = 0;
    LineSpan(starts, text, line, &b, &e);
    std::size_t i = b;
    while (i < e && text[i] == ' ') ++i;
    if (i == e || text[i] == '#') continue;  // blank or comment-only: not content
    if (i - b > keyColumn) {
      last = line;
      continue;
    }
    break;  // the next top-level token
  }
  return last + 1 < starts.size() ? starts[last + 1] : text.size();
}

std::string LineEnding(const std::string& text, const std::vector<std::size_t>& starts, std::size_t line) {
  const std::size_t end = line + 1 < starts.size() ? starts[line + 1] : text.size();
  return end >= 2 && text[end - 1] == '\n' && text[end - 2] == '\r' ? "\r\n" : "\n";
}

// The edit itself. `existing` is the file's text, or null when the file is missing.
Splice SpliceScalar(const std::string* existing, const std::string& section, const std::string& key, const ScalarEdit& edit, std::string* out) {
  const std::string& v = edit.text;
  if (existing == nullptr) {
    *out = section + ":\n  " + key + ": " + v + "\n";
    Splice s;
    s.ok = true;
    return s;
  }
  const std::string& text = *existing;
  std::vector<YAML::Node> docs;
  try {
    docs = YAML::LoadAll(text);
  } catch (const YAML::Exception&) {
    return Refused("unparseable");
  }
  if (docs.size() > 1) return Refused("multi_document");
  const std::vector<std::size_t> starts = LineStarts(text);
  const YAML::Node root = docs.empty() ? YAML::Node() : docs[0];

  auto appendSection = [&]() {
    std::string base = text;
    const std::string eol = text.find("\r\n") != std::string::npos ? "\r\n" : "\n";
    if (!base.empty() && base.back() != '\n') base += eol;
    *out = base + section + ":" + eol + "  " + key + ": " + v + eol;
    Splice s;
    s.ok = true;
    return s;
  };

  Splice result;
  if (!root.IsDefined() || root.IsNull()) {
    result = appendSection();  // an empty file, or one of comments only
  } else {
    if (!root.IsMap()) return Refused("not_a_mapping");
    if (root.Style() == YAML::EmitterStyle::Flow) return Refused("root_flow_style");
    std::vector<std::pair<YAML::Node, YAML::Node>> sectionHits;
    for (YAML::const_iterator it = root.begin(); it != root.end(); ++it) {
      if (it->first.IsScalar() && it->first.Scalar() == section) sectionHits.emplace_back(it->first, it->second);
    }
    if (sectionHits.size() > 1) return Refused("duplicate_section");
    if (sectionHits.empty()) {
      result = appendSection();
    } else {
      const YAML::Node sectionKey = sectionHits[0].first;
      const YAML::Node sectionNode = sectionHits[0].second;
      std::size_t keyOffset = 0;
      if (!OffsetOf(starts, text, sectionKey.Mark(), &keyOffset) || text.compare(keyOffset, section.size(), section) != 0) {
        return Refused("position_mismatch");
      }
      const std::size_t keyLine = static_cast<std::size_t>(sectionKey.Mark().line);
      const std::size_t keyColumn = static_cast<std::size_t>(sectionKey.Mark().column);
      std::size_t lb = 0, le = 0;
      LineSpan(starts, text, keyLine, &lb, &le);

      if (sectionNode.IsNull()) {
        // `graphics:` with nothing under it: the child goes on the next line. An explicit null (`graphics: ~`) is a value.
        std::size_t p = keyOffset + section.size();
        while (p < le && (text[p] == ' ' || text[p] == '\t')) ++p;
        if (p >= le || text[p] != ':') return Refused("position_mismatch");
        if (!OnlySpaceOrComment(text, p + 1, le)) return Refused("section_not_a_mapping");
        const std::string eol = LineEnding(text, starts, keyLine);
        std::string base = text;
        std::size_t at = keyLine + 1 < starts.size() ? starts[keyLine + 1] : text.size();
        std::string add = std::string(keyColumn + 2, ' ') + key + ": " + v + eol;
        if (at == text.size() && (text.empty() || text.back() != '\n')) add = eol + add;
        base.insert(at, add);
        *out = base;
        result.ok = true;
      } else {
        if (!sectionNode.IsMap()) return Refused("section_not_a_mapping");
        if (sectionNode.Style() == YAML::EmitterStyle::Flow) return Refused("section_flow_style");
        std::vector<std::pair<YAML::Node, YAML::Node>> hits;
        std::vector<YAML::Node> firstKey;
        for (YAML::const_iterator it = sectionNode.begin(); it != sectionNode.end(); ++it) {
          if (firstKey.empty()) firstKey.push_back(it->first);
          if (it->first.IsScalar() && it->first.Scalar() == key) hits.emplace_back(it->first, it->second);
        }
        if (hits.size() > 1) return Refused("duplicate_key");
        if (hits.empty()) {
          if (firstKey.empty() || !firstKey[0].IsScalar()) return Refused("position_mismatch");
          const std::size_t childColumn = static_cast<std::size_t>(firstKey[0].Mark().column);
          const std::string eol = LineEnding(text, starts, keyLine);
          std::string base = text;
          const std::size_t at = BlockEnd(starts, text, keyLine, keyColumn);
          std::string add = std::string(childColumn, ' ') + key + ": " + v + eol;
          if (at == text.size() && (text.empty() || text.back() != '\n')) add = eol + add;
          base.insert(at, add);
          *out = base;
          result.ok = true;
        } else {
          const YAML::Node valueKey = hits[0].first;
          const YAML::Node valueNode = hits[0].second;
          if (valueNode.IsMap() || valueNode.IsSequence()) return Refused("value_not_a_scalar");
          std::size_t kOff = 0;
          if (!OffsetOf(starts, text, valueKey.Mark(), &kOff) || text.compare(kOff, key.size(), key) != 0) {
            return Refused("position_mismatch");
          }
          const std::size_t kLine = static_cast<std::size_t>(valueKey.Mark().line);
          std::size_t kb = 0, ke = 0;
          LineSpan(starts, text, kLine, &kb, &ke);
          std::size_t colon = kOff + key.size();
          while (colon < ke && (text[colon] == ' ' || text[colon] == '\t')) ++colon;
          if (colon >= ke || text[colon] != ':') return Refused("position_mismatch");
          if (valueNode.IsNull() && OnlySpaceOrComment(text, colon + 1, ke)) {
            // `hand_flip:` with no value: the value goes right after the colon.
            std::string base = text;
            base.insert(colon + 1, " " + v);
            *out = base;
            result.ok = true;
          } else {
            if (!valueNode.IsScalar()) return Refused("value_not_a_scalar");
            if (valueNode.Tag() != "?" && valueNode.Tag() != "!") return Refused("value_not_plain");  // a tag, e.g. !!str
            if (ScalarMatches(valueNode, edit)) {
              *out = text;
              result.ok = true;
              result.unchanged = true;
              return result;
            }
            std::size_t vOff = 0;
            if (!OffsetOf(starts, text, valueNode.Mark(), &vOff)) return Refused("position_mismatch");
            if (static_cast<std::size_t>(valueNode.Mark().line) != kLine || vOff <= colon) return Refused("value_not_on_key_line");
            if (vOff >= ke) return Refused("position_mismatch");
            std::size_t from = 0, to = 0;
            const char first = text[vOff];
            if (first == '"' || first == '\'') {
              // A quoted value: only what is inside the quotes changes, the quotes stay.
              std::size_t i = vOff + 1;
              bool closed = false;
              for (; i < ke; ++i) {
                if (first == '"' && text[i] == '\\') {
                  ++i;
                  continue;
                }
                if (text[i] == first) {
                  if (first == '\'' && i + 1 < ke && text[i + 1] == '\'') {
                    ++i;
                    continue;
                  }
                  closed = true;
                  break;
                }
              }
              if (!closed) return Refused("multiline_scalar");
              from = vOff + 1;
              to = i;
            } else {
              if (std::string("&*!|>[{%@`").find(first) != std::string::npos) return Refused("value_not_plain");
              const std::string& scalar = valueNode.Scalar();
              if (scalar.empty() || text.compare(vOff, scalar.size(), scalar) != 0) return Refused("position_mismatch");
              const std::size_t after = vOff + scalar.size();
              if (after < ke && text[after] != ' ' && text[after] != '\t' && text[after] != '#') return Refused("multiline_scalar");
              from = vOff;
              to = after;
            }
            std::string edited = text;
            edited.replace(from, to - from, v);
            *out = edited;
            result.ok = true;
          }
        }
      }
    }
  }
  if (!result.ok) return Refused("position_mismatch");
  if (!ReadsBack(*out, section, key, edit)) {
    out->clear();
    return Refused("verify_failed");
  }
  return result;
}

// One mutex per config file for the whole of a Set: the read of the file, the splice, the write of `<file>.tmp`, the rename and the update of
// the in-memory tree. Two callers in one process (the Quest's texture poller and hand pose poller each edit config.yaml from a thread
// of their own) would otherwise share the one `<file>.tmp` and each read the file before the other published its edit, so the later
// rename dropped the earlier one's key. A unique temporary name alone does not cure that: both would still read the same old text,
// and the second rename would still replace the first one's result with a file that lacks its key. Holding one lock across read,
// splice and rename makes the second caller read what the first published.
//
// The key is the file's canonical path (its directory canonical, see LockKeyFor), so `a/config.yaml` and `a/../a/config.yaml` take
// the same mutex.
// Mutexes are never removed: the registry is a leaked heap object (no destructor to race a poller still running at exit, no
// load-time initializer for the Quest's check), and one entry is a few dozen bytes per distinct config path a process ever edits.
// Another process editing the same file is not covered (that needs an advisory file lock, which is a different change).
struct FileLocks {
  std::mutex registry;
  std::map<std::string, std::unique_ptr<std::mutex>> byFile;
};

// The lock's key: the file's directory made canonical (links and `..` resolved) plus the file's own name. The file itself is never
// opened to find its identity: on Windows a canonicalizing open of the target, taken by a caller waiting for the lock while the holder
// renames the new text over it, makes the holder's rename fail with access denied (measured under Wine: rename_failed err=13).
// A link standing at the file's own name is the thing Set's rename replaces, so the name belongs in the key as written.
std::string LockKeyFor(const std::string& boundFile) {
  std::error_code ec;
  const fs::path absolute = fs::absolute(fs::path(boundFile), ec);
  if (ec) return fs::path(boundFile).lexically_normal().string();
  const fs::path directory = fs::weakly_canonical(absolute.parent_path(), ec);  // the directory is read, never the file
  std::string key = ec ? absolute.lexically_normal().string() : (directory / absolute.filename()).string();
#ifdef _WIN32
  // Windows paths compare without regard to case; the mutex must not depend on how the caller cased the path.
  for (char& c : key) {
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
  }
#endif
  return key;
}

std::mutex& MutexFor(const std::string& boundFile) {
  static FileLocks* const locks = new FileLocks;  // leaked on purpose, see above
  const std::string key = LockKeyFor(boundFile);
  const std::lock_guard<std::mutex> hold(locks->registry);
  std::unique_ptr<std::mutex>& slot = locks->byFile[key];
  if (!slot) slot = std::make_unique<std::mutex>();
  return *slot;
}

int RealRename(const char* from, const char* to) {
  std::error_code ec;
  fs::rename(fs::path(from), fs::path(to), ec);
  return ec ? (ec.value() != 0 ? ec.value() : EIO) : 0;
}

config_testing::RenameFn g_rename = nullptr;
config_testing::AfterMemoryUpdateFn g_afterMemoryUpdate = nullptr;

constexpr std::uintmax_t kMaxEditableBytes = 1024 * 1024;

}  // namespace

namespace config_testing {
void SetRenameForTest(RenameFn rename) { g_rename = rename; }
void SetAfterMemoryUpdateForTest(AfterMemoryUpdateFn hook) { g_afterMemoryUpdate = hook; }
}  // namespace config_testing

ConfigSetResult NevrConfig::Set(const std::string& path, bool value) noexcept {
  ScalarEdit edit;
  edit.text = BoolText(value);
  edit.isBool = true;
  edit.boolValue = value;
  return SetScalar(path, edit);
}

ConfigSetResult NevrConfig::Set(const std::string& path, const std::string& value) noexcept {
  ScalarEdit edit;
  edit.text = value;
  if (!IsPlainWord(value)) {
    ConfigSetResult r;
    r.reason = "value_not_a_plain_word";
    return r;
  }
  return SetScalar(path, edit);
}

ConfigSetResult NevrConfig::SetScalar(const std::string& path, const ScalarEdit& edit) noexcept {
  ConfigSetResult r;
  try {
    if (boundFile_.empty()) {
      r.reason = "no_file";
      return r;
    }
    const std::size_t dot = path.find('.');
    if (dot == std::string::npos || dot == 0 || dot + 1 >= path.size() || path.find('.', dot + 1) != std::string::npos) {
      r.reason = "unsupported_path";
      return r;
    }
    const std::string section = path.substr(0, dot);
    const std::string key = path.substr(dot + 1);

    // From here to the return, no other caller in this process edits the same file (see MutexFor). The in-memory update at the end is
    // inside it too, so two threads calling Set on one NevrConfig object do not race on its tree either.
    const std::lock_guard<std::mutex> fileLock(MutexFor(boundFile_));

    std::error_code ec;
    const fs::file_status status = fs::status(fs::path(boundFile_), ec);
    std::string text;
    const std::string* existing = nullptr;
    if (status.type() == fs::file_type::not_found) {
      existing = nullptr;
    } else if (ec) {
      r.reason = "unreadable";
      r.err = ec.value();
      return r;
    } else if (status.type() != fs::file_type::regular) {
      r.reason = "not_a_regular_file";
      return r;
    } else {
      const std::uintmax_t size = fs::file_size(fs::path(boundFile_), ec);
      if (ec) {
        r.reason = "unreadable";
        r.err = ec.value();
        return r;
      }
      if (size > kMaxEditableBytes) {
        r.reason = "too_large";
        return r;
      }
      std::ifstream in(boundFile_, std::ios::binary);
      if (!in) {
        r.reason = "unreadable";
        r.err = errno != 0 ? errno : EIO;
        return r;
      }
      std::stringstream ss;
      ss << in.rdbuf();
      if (in.bad()) {
        r.reason = "unreadable";
        r.err = EIO;
        return r;
      }
      text = ss.str();
      existing = &text;
    }

    std::string edited;
    const Splice splice = SpliceScalar(existing, section, key, edit, &edited);
    if (!splice.ok) {
      r.reason = splice.reason;
      return r;
    }
    if (!splice.unchanged) {
      const std::string temporary = boundFile_ + ".tmp";
      {
        std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
        if (!out) {
          r.reason = "write_failed";
          r.err = errno != 0 ? errno : EIO;
          return r;
        }
        out.write(edited.data(), static_cast<std::streamsize>(edited.size()));
        out.flush();
        if (!out) {
          out.close();
          std::error_code ignore;
          fs::remove(fs::path(temporary), ignore);
          r.reason = "write_failed";
          r.err = errno != 0 ? errno : EIO;
          return r;
        }
      }
      const int rc = (g_rename != nullptr ? g_rename : &RealRename)(temporary.c_str(), boundFile_.c_str());
      if (rc != 0) {
        std::error_code ignore;
        fs::remove(fs::path(temporary), ignore);
        r.reason = "rename_failed";
        r.err = rc;
        return r;
      }
      r.wrote = true;
    }
    r.ok = true;
    r.reason = "";
    if (impl_) {
      try {
        if (edit.isBool) {
          impl_->root[section][key] = edit.boolValue;
        } else {
          impl_->root[section][key] = edit.text;
        }
      } catch (const YAML::Exception&) {
        // The file holds the value; the in-memory tree keeps what it had.
      }
    }
    if (g_afterMemoryUpdate != nullptr) g_afterMemoryUpdate();  // test seam: still inside the file's lock
    return r;
  } catch (const std::exception&) {
    r.ok = false;
    r.reason = "write_failed";
    r.err = EIO;
    return r;
  }
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
