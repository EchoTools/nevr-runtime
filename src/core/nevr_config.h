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

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace nevr {

/// Thrown by the parsing entry points on ANY failure: file not readable, YAML
/// parse error, an unknown top-level key, a malformed `plugins` list, or an
/// unresolved `${VAR:?}` interpolation. Internal to the runtime — it is caught
/// at the (future) consumer call sites and never crosses a DLL export boundary.
class NevrConfigError : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};

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
};

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

  /// Production policy wrapper (no consumer in S2). On failure:
  ///   server mode -> FatalError(...) — delegates to the registered handler
  ///                  (ForceFatalExit in a real server; the N4 path);
  ///   client mode -> logs a Warning and returns an empty config (Empty()==true).
  static NevrConfig LoadFromFileOrFail(const std::string& path, bool is_server);

  /// Typed getters. `path` is dotted, e.g. "services.serverdb". Return nullopt
  /// when the path is absent or the scalar cannot be converted to the type.
  std::optional<std::string> GetString(const std::string& path) const;
  std::optional<bool> GetBool(const std::string& path) const;
  std::optional<std::int64_t> GetInt(const std::string& path) const;
  std::optional<double> GetFloat(const std::string& path) const;
  /// A sequence at `path` as strings; a lone scalar becomes a single element.
  std::vector<std::string> GetStringList(const std::string& path) const;

  const std::vector<PluginSpec>& Plugins() const { return plugins_; }
  bool Empty() const { return empty_; }

 private:
  struct Impl;                       // holds the interpolated yaml-cpp tree (.cpp only)
  std::unique_ptr<Impl> impl_;
  std::vector<PluginSpec> plugins_;
  bool empty_ = true;
};

}  // namespace nevr
