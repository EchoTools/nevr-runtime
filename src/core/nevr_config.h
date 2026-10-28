// nevr_config.h — the typed reader for NEVR's modern `config.yaml` (N133).
//
// One `config.yaml`, parsed once by yaml-cpp into this object, owns every NEVR
// setting (the cutover away from the game's JSON / g_localConfig reads). This
// header deliberately does NOT include yaml-cpp: the parsed tree lives behind a
// pImpl in nevr_config.cpp, so a consumer — or a module — that includes this
// header pulls in no yaml-cpp dependency. yaml-cpp links into nevr_runtime only.
//
// Membership: core/ ("we wrote this"). No consumer is wired yet (S2 of the
// cutover); this compiles and is unit-tested in isolation (test_nevr_config).

#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace nevr {

/// Why a load failed, as a class a caller can log without repeating the parser's text. The text (`what()`)
/// echoes the file's own words (a key, a line of YAML), which the Quest's sentinel log must not carry.
enum class NevrConfigErrorKind {
  kOther,
  kUnreadable,
  kYamlSyntax,
  kNotAMapping,
  kUnknownTopLevelKey,
  kBadPlugins,
  kUnsetRequiredVariable,
  kNestingTooDeep,
  kTooManyNodes,
};

/// Bounds on the walks over a file's YAML tree, per load. An alias can make the tree a cycle (`x-a: &a [*a]`) or
/// expand it exponentially (nested aliases of aliases), and a walk that follows it without a bound exhausts the
/// stack or runs for minutes, in the Quest's constructor as well as on the PC. Exceeding either bound rejects the
/// file (kNestingTooDeep, kTooManyNodes).
///
/// Depth: the root mapping is depth 0 and a scalar inside k nested sequences under a top-level key is depth k + 1.
/// 64 is far deeper than a settings file nests (the sections hold scalars; only plugin `args` nest, by dotted
/// key), and it keeps the walk to 64 frames where the unbounded walk overflowed the stack at 21,792 on a Linux host.
inline constexpr std::size_t kMaxConfigDepth = 64;
/// Nodes visited, counting each visit of an aliased node: 100,000 is three times what a 64 KiB file (the Quest's
/// size cap) can hold as plain flow entries (`[a,a,a]` is 2 bytes a node, 32,768 nodes), while 434 bytes of nested
/// aliases (8 levels of ten references) expand to 10^8 visits.
inline constexpr std::size_t kMaxConfigNodes = 100000;

/// A stable, value-free token for a kind: "unreadable", "yaml_syntax", "not_a_mapping", "unknown_top_level_key",
/// "bad_plugins", "unset_required_variable", "nesting_too_deep", "too_many_nodes", and "invalid_config" for kOther.
const char* ReasonToken(NevrConfigErrorKind kind);

/// Thrown by the parsing entry points on ANY failure: file not readable, YAML
/// parse error, an unknown top-level key, a malformed `plugins` list, or an
/// unresolved `${VAR:?}` interpolation. Internal to the runtime — it is caught
/// at the consumer call sites and never crosses a DLL export boundary.
class NevrConfigError : public std::runtime_error {
 public:
  explicit NevrConfigError(const std::string& what, NevrConfigErrorKind kind = NevrConfigErrorKind::kOther,
                           int line = 0, int column = 0)
      : std::runtime_error(what), kind_(kind), line_(line), column_(column) {}
  NevrConfigErrorKind Kind() const noexcept { return kind_; }
  int Line() const noexcept { return line_; }
  int Column() const noexcept { return column_; }

 private:
  NevrConfigErrorKind kind_;
  int line_;
  int column_;
};

/// PC-only diagnostic fields for a rejected file; excludes the parser's text.
std::string ConfigRejectionDetail(const NevrConfigError& error);

/// One entry of the ordered `plugins:` list. `args` is flattened to dotted keys
/// (e.g. `foo.bar` for a nested map) with interpolated scalar string values, so
/// the loader can pass them without re-exposing yaml-cpp.
struct PluginSpec {
  std::string name;                          // required
  std::string file;                          // dll filename; defaults to name + ".dll"
  bool enabled = true;
  bool required = false;
  std::string target;                        // optional deployment-target hint
  std::map<std::string, std::string> args;   // dotted-key -> interpolated scalar
  bool early = false;                        // load before the game reads its data (#496)
};

/// The kind of node a config path names (NevrConfig::Kind). `kNull` is a key written with no value (or `~`, `null`).
enum class NodeKind { kAbsent, kNull, kScalar, kSequence, kMapping };

class NevrConfig {
 public:
  NevrConfig();
  ~NevrConfig();
  NevrConfig(NevrConfig&&) noexcept;
  NevrConfig& operator=(NevrConfig&&) noexcept;
  NevrConfig(const NevrConfig&) = delete;
  NevrConfig& operator=(const NevrConfig&) = delete;

  /// Parse from a YAML string / file. Throw NevrConfigError on any failure.
  /// An empty document is valid and yields an empty config (no plugins).
  static NevrConfig LoadFromString(const std::string& yaml);
  static NevrConfig LoadFromFile(const std::string& path);

  /// Production policy wrapper (PC only: defined in nevr_config_policy.cpp, which the Quest does not build).
  /// On failure:
  ///   server mode -> FatalError(...) — delegates to the registered handler
  ///                  (ForceFatalExit in a real server; the N4 path);
  ///   client mode -> logs a Warning and returns an empty config (Empty()==true).
  /// On success it logs LogUnsetBareVars for the loaded config.
  static NevrConfig LoadFromFileOrFail(const std::string& path, bool is_server);

  /// Typed getters. `path` is dotted, e.g. "services.serverdb". Return nullopt
  /// when the path is absent or the scalar cannot be converted to the type.
  /// When `hadUnsetBare` is non-null it is set to whether the scalar referenced a bare ${VAR} that
  /// is not set (its text stays in the returned value as written). That is exact: a literal "${"
  /// written with the $${ escape, or inside a set variable's value, does not count.
  std::optional<std::string> GetString(const std::string& path, bool* hadUnsetBare = nullptr) const;
  /// What `path` names in the file. The typed getters give nullopt for everything but a scalar of the right type, so this is
  /// how a caller tells "not set" from "set to something that is not a value" (a key with no value, a mapping, a list).
  NodeKind Kind(const std::string& path) const;
  /// The scalar keys of the mapping at `path`, in file order; empty when `path` is absent or is not a mapping. A key that is
  /// not a scalar (a mapping or a sequence used as a key) is left out. Keys are returned as written, never interpolated.
  std::vector<std::string> MappingKeys(const std::string& path) const;
  std::optional<bool> GetBool(const std::string& path) const;
  std::optional<std::int64_t> GetInt(const std::string& path) const;
  std::optional<double> GetFloat(const std::string& path) const;
  /// A sequence at `path` as strings; a lone scalar becomes a single element.
  std::vector<std::string> GetStringList(const std::string& path) const;

  const std::vector<PluginSpec>& Plugins() const { return plugins_; }
  bool Empty() const { return empty_; }
  /// Names of unset bare ${VAR} references found at load, each once, sorted. Their text stays in
  /// the value as written; the loader does not log them (LogUnsetBareVars, or the caller, does).
  const std::vector<std::string>& UnsetBareVars() const { return unsetBareVars_; }
  /// Mistakes in the file's shape that the parser keeps going past (#583). DuplicateKeyCount: a key that repeats in
  /// one mapping (the first value is the one read). UnknownKeyCount: a key under a section in KnownConfigKeyPaths()
  /// that no reader reads (ignored). Counts only: the names and values stay out, as the Quest's log requires.
  std::size_t DuplicateKeyCount() const { return duplicateKeys_; }
  std::size_t UnknownKeyCount() const { return unknownKeys_; }

 private:
  struct Impl;                       // holds the interpolated yaml-cpp tree (.cpp only)
  std::unique_ptr<Impl> impl_;
  std::vector<PluginSpec> plugins_;
  bool empty_ = true;
  std::vector<std::string> unsetBareVars_;
  std::size_t duplicateKeys_ = 0;
  std::size_t unknownKeys_ = 0;
};

/// Every dotted path a reader of either build reads under a known section, sorted (#583): the one list the unknown-key
/// count checks against. A section not listed here (plugins, guilds, profiles, version, x-* extensions) is free-form
/// and is not checked.
std::vector<std::string> KnownConfigKeyPaths();

/// PC only (defined in nevr_config_policy.cpp): logs one Warning per name in cfg.UnsetBareVars(), through Log().
/// The parser itself logs nothing, so the Quest can build it without the PC logger.
void LogUnsetBareVars(const NevrConfig& cfg);

/// PC only (defined in nevr_config_policy.cpp): one Warning with the duplicate and unknown-key counts when either is
/// non-zero (#583). Names and values are not logged.
void LogShapeWarnings(const NevrConfig& cfg);

/// Case-insensitive true/yes/on/1 and false/no/off/0; nullopt for anything else. The plugin-list
/// flags and the flat-key consumers use it; NevrConfig::GetBool also falls back to yaml-cpp's
/// boolean spellings.
std::optional<bool> ParseBool(const std::string& s);

}  // namespace nevr
