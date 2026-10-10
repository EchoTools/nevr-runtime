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
#include "runtime/lifecycle/stable_string_pool.h"
#include "runtime/lifecycle/cli.h"        // g_customConfigPath, g_isServer (drags core/pch.h -> windows.h, now NOMINMAX)
#include "runtime/lifecycle/crash_recovery.h"  // ServerFatal (S4a fail-loud)
#include "runtime/ext/plugin_load_plan.h"        // PluginLoadItem / NevrCfgPluginLoadPlan (N134 S6)
#include "runtime/ext/plugin_load_plan_build.h"  // BuildLoadPlan (pure builder)
#include "abi/echovr_functions.h"         // EchoVR::g_GameBaseAddress
#include "core/logging.h"                 // Log()
#include "core/nevr_config.h"
#include "generated/nevr_builtin_defaults.h"  // build-tree only; values from env/.env at configure

#ifdef NEVR_TEST_HOOKS
#include "runtime/tests/service_config_test_hooks.h"
#endif

#include <fstream>
#include <cstdlib>
#include <exception>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#ifdef NEVR_TEST_HOOKS
#include <stdexcept>
#endif

namespace {

#ifdef NEVR_TEST_HOOKS
const nevr::NevrConfig* g_testConfig = nullptr;
const nevr_cfg::FlatDefaults* g_testDefaults = nullptr;
bool g_testInputsSet = false;
bool g_testServerMode = false;
std::string_view g_failInternAccessor;
std::string_view g_throwAccessor;
#endif

bool IsServerMode() {
#ifdef NEVR_TEST_HOOKS
  if (g_testInputsSet) return g_testServerMode;
#endif
  return g_isServer != FALSE;
}

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
// Load policy (S4a): a MISSING config.yaml is non-fatal in both modes — warn and
// yield an empty config (every migrated key then resolves to its hardcoded
// default; "no config" is a legitimate defaults-only run). But a config.yaml that
// EXISTS and is malformed OR references an unset required ${VAR:?} secret FAILS
// LOUD in server mode via ServerFatal -> ForceFatalExit — never a silent degrade
// to no-login on an empty secret (HARD RISK #3 / N115). ServerFatal warns and
// returns in client mode, so the client path stays warn+empty as before.
//
// Ordering — why the S3 hazard (a fatal that pre-dates the handler install could
// block on an invisible MessageBox) does NOT apply here: ServerFatal terminates
// via ForceFatalExit (TerminateProcess), never MessageBoxA. And by first access
// the whole install chain is up regardless: first access is boot.cpp
// PreprocessCommandLineHook, which runs after Initialize() installed every hook,
// AFTER g_isServer is set (boot.cpp:39) and AFTER InstallFatalErrorHandler()
// (boot.cpp:53), and the lazy load stays after the CLI loop so -config-path
// (g_customConfigPath) is honoured. The secret itself is not accessed until ws
// login, far later still. So an unset ${NEVR_PASSWORD:?} fails loud at first
// access (the socket_uri read, boot.cpp) instead of ever reaching ws_bridge.
const nevr::NevrConfig& NevrCfg() {
#ifdef NEVR_TEST_HOOKS
  if (g_testInputsSet && g_testConfig != nullptr) return *g_testConfig;
#endif
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
      // Server: fail loud (ServerFatal -> ForceFatalExit). Client: ServerFatal
      // warns and returns, then we fall through to the warn+empty client path.
      ServerFatal("config.yaml at %s is invalid: %s — refusing to run a server on a "
                  "broken or incomplete config (an unset required secret must fail loud, "
                  "never silently degrade to no login)",
                  path.c_str(), e.what());
      Log(EchoVR::LogLevel::Warning,
          "[NEVR.CONFIG] continuing with built-in defaults after config.yaml rejection (client mode)");
      return nevr::NevrConfig();
    }
  }();
  return cfg;
}

// --- build-time defaults ---------------------------------------------------
// The four keys a player needs to reach the service without any config file, embedded at
// configure time from the environment / .env (cmake/nevr_builtin_defaults.cmake). Applied
// at LOOKUP time under the config.yaml value (nevr_cfg::LookupFlatWithDefaults), so a
// config.yaml can override any of them and a rejected or absent file still leaves them in
// place. Client mode only: a dedicated server has always been configured explicitly, and
// must not silently start a bridge or authenticate with an embedded key. Only key NAMES are
// logged, never values (the two keys are public by design (#76), but socket_uri may carry a token).
const nevr_cfg::FlatDefaults& BuiltinDefaults() {
#ifdef NEVR_TEST_HOOKS
  if (g_testInputsSet) {
    // The injected defaults go through the same gate as the embedded ones.
    static nevr_cfg::FlatDefaults gated;
    std::vector<nevr_cfg::EmbeddedDefault> entries;
    if (g_testDefaults != nullptr) {
      for (const auto& kv : *g_testDefaults) entries.push_back({kv.first.c_str(), kv.second.c_str()});
    }
    gated = nevr_cfg::SelectBuiltinDefaults(g_testServerMode, entries.data(), entries.size(), nullptr,
                                            nullptr);
    return gated;
  }
#endif
  static const nevr_cfg::FlatDefaults defaults = []() {
    const nevr_cfg::EmbeddedDefault kEmbedded[] = {
        {"nevr_socket_uri", nevr_builtin::kSocketUri},
        {"nevr_http_uri", nevr_builtin::kHttpUri},
        {"nevr_http_key", nevr_builtin::kPublicApiKey},
        {"nevr_server_key", nevr_builtin::kPublicSocketKey},
    };
    std::string embedded;
    std::string missing;
    nevr_cfg::FlatDefaults d = nevr_cfg::SelectBuiltinDefaults(
        IsServerMode(), kEmbedded, sizeof(kEmbedded) / sizeof(kEmbedded[0]), &embedded, &missing);
    if (IsServerMode()) {
      Log(EchoVR::LogLevel::Info,
          "[NEVR.CONFIG] built-in defaults are not applied in server mode (config.yaml is required)");
      return d;
    }
    Log(EchoVR::LogLevel::Info, "[NEVR.CONFIG] built-in defaults embedded in this build: %s",
        embedded.empty() ? "(none)" : embedded.c_str());
    if (!missing.empty()) {
      Log(EchoVR::LogLevel::Info,
          "[NEVR.CONFIG] not embedded (config.yaml must supply them): %s", missing.c_str());
    }
    return d;
  }();
  return defaults;
}

// The runtime's environment overrides (#76): NEVR_API_KEY and NEVR_SOCKET_KEY, read once at
// start-up, above config.yaml and the built-in defaults. In both modes. Names logged, never values.
const nevr_cfg::FlatEnvOverrides& EnvOverrides() {
  static const nevr_cfg::FlatEnvOverrides overrides = []() {
    nevr_cfg::FlatEnvOverrides o = nevr_cfg::ReadFlatEnvOverrides([](const char* name) -> std::optional<std::string> {
      char buf[1024];
      const DWORD len = GetEnvironmentVariableA(name, buf, sizeof(buf));
      if (len == 0 || len >= sizeof(buf)) return std::nullopt;
      return std::string(buf, len);
    });
    std::string names;
    for (const nevr_cfg::FlatEnvVar& var : nevr_cfg::kFlatEnvVars) {
      if (o.count(var.flatKey) == 0) continue;
      if (!names.empty()) names += ", ";
      names += std::string(var.envName) + " (" + nevr_cfg::FlatKeyToYamlPath(var.flatKey) + ")";
    }
    Log(EchoVR::LogLevel::Info, "[NEVR.CONFIG] environment overrides: %s", names.empty() ? "(none)" : names.c_str());
    return o;
  }();
  return overrides;
}

// One lookup for every flat key: environment override, else config.yaml value, else the
// embedded default.
std::optional<std::string> Flat(const std::string& flatKey) {
#ifdef NEVR_TEST_HOOKS
  if (g_testInputsSet && g_testConfig != nullptr) {
    return nevr_cfg::LookupFlatWithDefaults(*g_testConfig, BuiltinDefaults(), flatKey);
  }
#endif
  return nevr_cfg::LookupFlatLayered(NevrCfg(), EnvOverrides(), BuiltinDefaults(), flatKey);
}

[[noreturn]] void FailAccessor(const char* accessor, const char* status, std::size_t count,
                               std::size_t liveBytes) {
  Log(EchoVR::LogLevel::Error,
      "[NEVR.CONFIG] C accessor failed accessor=%s status=%s strings=%zu live_bytes=%zu",
      accessor, status, count, liveBytes);
  ForceFatalExit(1);
  std::abort();
}

const char* InternCStr(std::string_view value, const char* accessor) {
  nevr::lifecycle::InternResult result{};
#ifdef NEVR_TEST_HOOKS
  if (g_failInternAccessor == accessor) {
    result = {nevr::lifecycle::InternStatus::kAllocationFailure, nullptr, 7, 13};
  } else
#endif
  {
    result = nevr::lifecycle::InternStableCStr(value);
  }
  if (result.status != nevr::lifecycle::InternStatus::kSuccess || result.pointer == nullptr) {
    FailAccessor(accessor, nevr::lifecycle::InternStatusName(result.status), result.stringCount,
                 result.liveBytes);
  }
  return result.pointer;
}

template <typename Body>
const char* AccessorBoundary(const char* accessor, Body body) {
  try {
#ifdef NEVR_TEST_HOOKS
    if (g_throwAccessor == accessor) throw std::runtime_error("injected accessor exception");
#endif
    return body();
  } catch (const std::exception&) {
    FailAccessor(accessor, "exception", 0, 0);
  }
}

}  // namespace

#ifdef NEVR_TEST_HOOKS
namespace nevr::lifecycle::test {

void SetAccessorInputs(const nevr::NevrConfig* config, const nevr_cfg::FlatDefaults* defaults,
                       bool serverMode) {
  g_testConfig = config;
  g_testDefaults = defaults;
  g_testServerMode = serverMode;
  g_testInputsSet = true;
}

void FailInternAtAccessor(std::string_view accessor) { g_failInternAccessor = accessor; }
void ThrowAtAccessor(std::string_view accessor) { g_throwAccessor = accessor; }

void ResetAccessorInputs() {
  g_testConfig = nullptr;
  g_testDefaults = nullptr;
  g_testInputsSet = false;
  g_testServerMode = false;
  g_failInternAccessor = {};
  g_throwAccessor = {};
}

}  // namespace nevr::lifecycle::test
#endif

// --- C accessors -----------------------------------------------------------

const char* NevrCfgGetFlat(const char* flatKey) {
  return AccessorBoundary("NevrCfgGetFlat", [&]() -> const char* {
    if (flatKey == nullptr) return nullptr;
    const std::optional<std::string> v = Flat(flatKey);
    if (!v) return nullptr;  // unmapped or absent
    return InternCStr(*v, "NevrCfgGetFlat");
  });
}

const char* NevrCfgGetFlatCsv(const char* flatKey) {
  return AccessorBoundary("NevrCfgGetFlatCsv", [&]() -> const char* {
    // List-shaped keys (guilds, regions): a yaml list or a scalar CSV both come
    // back as the "a,b" string gameserver builds into guilds=/regions= URL params.
    if (flatKey == nullptr) return nullptr;
    const std::optional<std::string> v = nevr_cfg::LookupFlatCsv(NevrCfg(), flatKey);
    if (!v) return nullptr;  // unmapped or absent
    return InternCStr(*v, "NevrCfgGetFlatCsv");
  });
}

const char* NevrCfgServiceHost(const char* flatServiceKey, int* outSource) {
  return AccessorBoundary("NevrCfgServiceHost", [&]() -> const char* {
    const nevr_cfg::ServiceHostResult r =
        nevr_cfg::ResolveServiceHost(NevrCfg(), flatServiceKey ? flatServiceKey : "");
    if (!r.value) {
      if (outSource != nullptr) *outSource = 2;
      return nullptr;  // caller uses its hardcoded default
    }
    const char* value = InternCStr(*r.value, "NevrCfgServiceHost");
    if (outSource != nullptr) {
      *outSource = r.source == nevr_cfg::HostSource::kPrimary ? 0 : 1;
    }
    return value;
  });
}

const char* NevrCfgRedirect(const char* result, const char* httpTargetJson, int bridgeActive,
                            unsigned bridgePort) {
  return AccessorBoundary("NevrCfgRedirect", [&]() -> const char* {
    if (result == nullptr) return nullptr;
    const std::optional<std::string> socketTarget = Flat("nevr_socket_uri");
    std::optional<std::string> httpTarget;
    if (httpTargetJson != nullptr && httpTargetJson[0] != '\0') httpTarget = std::string(httpTargetJson);

    const std::optional<std::string> redir =
        nevr_cfg::ResolveRedirect(result, socketTarget, httpTarget, bridgeActive != 0, bridgePort);
    if (!redir) return nullptr;
    return InternCStr(*redir, "NevrCfgRedirect");
  });
}

const char* NevrGameNativeDefault(const char* key) {
  return AccessorBoundary("NevrGameNativeDefault", [&]() -> const char* {
    if (key == nullptr) return nullptr;
    const std::optional<std::string> v = nevr_cfg::GameNativeDefault(key);
    if (!v) return nullptr;
    return InternCStr(*v, "NevrGameNativeDefault");
  });
}

const char* NevrCfgAutoRelay(unsigned bridgePort) {
  return AccessorBoundary("NevrCfgAutoRelay", [&]() -> const char* {
    const std::optional<std::string> socketTarget = Flat("nevr_socket_uri");
    if (!socketTarget || socketTarget->empty()) return nullptr;
    return InternCStr(std::string("ws://127.0.0.1:") + std::to_string(bridgePort), "NevrCfgAutoRelay");
  });
}

bool NevrCfgSocialFacadeEnabled() {
  return nevr_cfg::SocialFacadeEnabled(NevrCfg());
}

const char* NevrCfgGameNativeConfigJson() {
  return AccessorBoundary("NevrCfgGameNativeConfigJson", [&]() -> const char* {
    if (IsServerMode()) return nullptr;  // a dedicated server has no social layer to configure
    const std::optional<std::string> httpUri = Flat("nevr_http_uri");
    const std::optional<std::string> serverKey = Flat("nevr_server_key");
    if (!httpUri || !serverKey) return nullptr;
    const std::optional<std::string> json = nevr_cfg::BuildGameNativeConfigJson(*httpUri, *serverKey);
    if (!json) return nullptr;
    return InternCStr(*json, "NevrCfgGameNativeConfigJson");
  });
}

// N134 S6 — the plugin loader's config source. The impure half: reads the same
// config.yaml singleton (loaded + fail-loud-validated once, above) and hands the
// loader the ordered plan (disabled entries flagged) built by the pure BuildLoadPlan. Kept
// here (not in plugin_load_plan.cpp) so the pure builder stays singleton-free and
// the test links it without dragging the singleton/Windows/Log surface.
std::vector<PluginLoadItem> NevrCfgPluginLoadPlan() {
  return nevr_plugincfg::BuildLoadPlan(NevrCfg());
}
