// service_config.cpp — see service_config.h. The IMPURE half of the config.cpp
// cutover (N133 S3): the config.yaml singleton, its file discovery, string
// interning for stable CHAR* returns, and the C-string accessors config.cpp
// calls. The resolution logic itself lives in service_map.* (pure, test-linked);
// this file only wires that logic to the loaded config + the game's stable-pointer
// contract.
//
// SKIP_PRECOMPILE_HEADERS + NOMINMAX BEFORE any windows.h (reached via cli.h ->
// core/pch.h): the core PCH pulls windows.h WITHOUT NOMINMAX, whose min/max
// macros break <optional>/<set>/<limits>. Mirrors nevr_config.cpp.

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "runtime/lifecycle/service_config.h"
#include "runtime/lifecycle/service_map.h"
#include "runtime/lifecycle/cli.h"        // g_customConfigPath (drags core/pch.h -> windows.h, now NOMINMAX)
#include "abi/echovr_functions.h"         // EchoVR::g_GameBaseAddress
#include "core/logging.h"                 // Log()
#include "core/nevr_config.h"

#include <fstream>
#include <mutex>
#include <optional>
#include <set>
#include <string>

namespace {

// --- config.yaml discovery -------------------------------------------------
// Mirrors LoadEarlyConfig's search for config.json (config.cpp): _local next to
// the game module, then one and two parents up. Additionally honours an explicit
// -config/-config-path directory when one was given (LoadEarlyConfig itself does
// not, but by first-access time g_customConfigPath is parsed, so we can). First
// existing file wins; "" if none is found.
std::string FindNevrConfigYamlPath() {
  CHAR moduleDir[MAX_PATH] = {0};
  GetModuleFileNameA(reinterpret_cast<HMODULE>(EchoVR::g_GameBaseAddress), moduleDir, MAX_PATH);
  if (CHAR* lastSlash = strrchr(moduleDir, '\\')) *(lastSlash + 1) = '\0';

  std::vector<std::string> candidates;

  // An explicit -config/-config-path override: look for config.yaml in the same
  // directory as the named path.
  if (g_customConfigPath[0] != '\0') {
    std::string cp(g_customConfigPath);
    const std::size_t slash = cp.find_last_of("\\/");
    const std::string dir = (slash == std::string::npos) ? std::string() : cp.substr(0, slash + 1);
    candidates.push_back(dir + "config.yaml");
  }

  const char* rel[] = {
      "%s_local\\config.yaml",
      "%s..\\_local\\config.yaml",
      "%s..\\..\\_local\\config.yaml",
  };
  for (const char* r : rel) {
    CHAR p[MAX_PATH] = {0};
    snprintf(p, MAX_PATH, r, moduleDir);
    candidates.push_back(p);
  }

  for (const std::string& c : candidates) {
    std::ifstream f(c, std::ios::binary);
    if (f.good()) return c;
  }
  return std::string();
}

// The one parsed config.yaml, loaded lazily on first access. Thread-safe init via
// the C++11 magic-static rule.
//
// Load policy for S3 is NON-FATAL on both a missing and a malformed file: it
// warns and yields an empty config (every migrated key then resolves to its
// hardcoded default). Rationale: (1) S3's key set has NO secrets, so HARD RISK
// #3 (fail-loud-or-send-empty, N115) does not apply here; (2) first access can
// pre-date the fatal-error handler install, and a fatal there would block on an
// invisible MessageBox (the very hang the primer warns about). S4/S5, which add
// secrets, can reintroduce fail-loud once handler-before-first-access is assured.
const nevr::NevrConfig& NevrCfg() {
  static const nevr::NevrConfig cfg = []() -> nevr::NevrConfig {
    const std::string path = FindNevrConfigYamlPath();
    if (path.empty()) {
      Log(EchoVR::LogLevel::Warning,
          "[NEVR.CONFIG] no config.yaml found — NEVR config keys use built-in defaults");
      return nevr::NevrConfig();
    }
    try {
      nevr::NevrConfig c = nevr::NevrConfig::LoadFromFile(path);
      Log(EchoVR::LogLevel::Info, "[NEVR.CONFIG] config.yaml loaded from: %s", path.c_str());
      return c;
    } catch (const nevr::NevrConfigError& e) {
      Log(EchoVR::LogLevel::Warning,
          "[NEVR.CONFIG] config.yaml parse error (%s) — NEVR config keys use built-in defaults",
          e.what());
      return nevr::NevrConfig();
    }
  }();
  return cfg;
}

// --- interning -------------------------------------------------------------
// The game keeps the CHAR* we return for the process lifetime (JsonValueAsString
// handed back tree-stable pointers). std::set nodes are address-stable across
// inserts and never erased here, so c_str() stays valid forever. Deduplicated by
// value, so the pool is bounded by the number of distinct configured strings.
const char* InternCStr(const std::string& s) {
  static std::mutex m;
  static std::set<std::string> pool;
  std::lock_guard<std::mutex> lk(m);
  return pool.insert(s).first->c_str();
}

}  // namespace

// --- C accessors -----------------------------------------------------------

const char* NevrCfgGetFlat(const char* flatKey) {
  if (flatKey == nullptr) return nullptr;
  const std::optional<std::string> v = nevr_cfg::LookupFlat(NevrCfg(), flatKey);
  if (!v) return nullptr;  // unmapped or absent
  return InternCStr(*v);   // present (possibly ""): caller applies its own [0] check
}

const char* NevrCfgServiceHost(const char* flatServiceKey, int* outSource) {
  const nevr_cfg::ServiceHostResult r =
      nevr_cfg::ResolveServiceHost(NevrCfg(), flatServiceKey ? flatServiceKey : "");
  if (outSource != nullptr) {
    switch (r.source) {
      case nevr_cfg::HostSource::kPrimary: *outSource = 0; break;
      case nevr_cfg::HostSource::kLoginFallback: *outSource = 1; break;
      case nevr_cfg::HostSource::kNone: *outSource = 2; break;
    }
  }
  if (!r.value) return nullptr;  // caller uses its hardcoded default
  return InternCStr(*r.value);
}

const char* NevrCfgRedirect(const char* result, const char* httpTargetJson, int bridgeActive,
                            unsigned bridgePort) {
  if (result == nullptr) return nullptr;
  const std::optional<std::string> socketTarget = nevr_cfg::LookupFlat(NevrCfg(), "nevr_socket_uri");
  std::optional<std::string> httpTarget;
  if (httpTargetJson != nullptr && httpTargetJson[0] != '\0') httpTarget = std::string(httpTargetJson);

  const std::optional<std::string> redir =
      nevr_cfg::ResolveRedirect(result, socketTarget, httpTarget, bridgeActive != 0, bridgePort);
  if (!redir) return nullptr;
  return InternCStr(*redir);
}

const char* NevrCfgAutoRelay(unsigned bridgePort) {
  const std::optional<std::string> socketTarget = nevr_cfg::LookupFlat(NevrCfg(), "nevr_socket_uri");
  if (!socketTarget || socketTarget->empty()) return nullptr;
  return InternCStr(std::string("ws://127.0.0.1:") + std::to_string(bridgePort));
}
