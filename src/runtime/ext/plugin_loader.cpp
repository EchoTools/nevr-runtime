#include "runtime/ext/plugin_loader.h"

#include <string>
#include <vector>

#include "core/globals.h"
#include "core/logging.h"

#include "runtime/lifecycle/cli.h"
#include "runtime/lifecycle/crash_recovery.h"  // ServerFatal
#include "runtime/hook/hook_guard.h"
#include "runtime/ext/plugin_load_plan.h"       // N134 S6: config-driven load plan

struct LoadedPlugin {
  HMODULE                     hModule;
  NvrPluginInfo               info;
  uint32_t                    api_version;
  uint32_t                    capabilities;  /* NvrPluginCapabilities bitmask; 0 = UNDECLARED */
  NvrPluginInit_fn            init;
  NvrPluginOnFrame_fn         on_frame;
  NvrPluginOnGameStateChange_fn on_state_change;
  NvrPluginShutdown_fn        shutdown;
  std::string                 path;
};

static std::vector<LoadedPlugin> g_plugins;

// N134 S6: required-aware load failure. A plugin the config declared
// `required: true` that fails to load or init is FATAL on a dedicated server
// (ServerFatal -> ForceFatalExit; the same N120 rule modules follow) and a hard
// warning on a client (ServerFatal warns and returns in client mode). An optional
// plugin (`required: false`, the default) warns and the loader continues. Caller
// `continue`s after this returns. `reason` is a fully-formed clause (no values
// that could be secrets — plugin args are NOT interpolated into it).
static void FailPluginLoad(const PluginLoadItem& item, const std::string& reason) {
  if (item.required) {
    ServerFatal("Required plugin %s (%s) failed: %s — a server must not run without a "
                "plugin it declared required",
                item.name.c_str(), item.file.c_str(), reason.c_str());
  } else {
    Log(EchoVR::LogLevel::Warning, "[NEVR.PLUGIN] %s (%s): %s — optional, continuing",
        item.name.c_str(), item.file.c_str(), reason.c_str());
  }
}

void LoadPlugins() {
  // Resolve plugins/ directory relative to echovr.exe (same dir as pnsradgameserver.dll)
  CHAR moduleDir[MAX_PATH] = {0};
  GetModuleFileNameA(reinterpret_cast<HMODULE>(EchoVR::g_GameBaseAddress), moduleDir, MAX_PATH);
  CHAR* lastSlash = strrchr(moduleDir, '\\');
  if (lastSlash) *(lastSlash + 1) = '\0';

  std::string pluginDir = std::string(moduleDir) + "plugins\\";

  // N134 S6: the plugin set is the ORDERED `plugins:` list in config.yaml, NOT a
  // glob of the deployed directory. Config is authoritative — an absent/empty
  // list loads NOTHING, and an orphan DLL left in plugins/ is ignored unless the
  // config names it. This is a deliberate hardening over the old "load every
  // *.dll found" discovery, whose failure mode was N89: a stale DLL silently
  // taking over for an entire run because it happened to sit in the directory.
  const std::vector<PluginLoadItem> plan = NevrCfgPluginLoadPlan();
  if (plan.empty()) {
    Log(EchoVR::LogLevel::Info,
        "[NEVR.PLUGIN] no plugins configured in config.yaml — loading none");
    return;
  }

  Log(EchoVR::LogLevel::Info,
      "[NEVR.PLUGIN] %zu plugin(s) configured; loading in list order from %s",
      plan.size(), pluginDir.c_str());

  // Build init context (shared; the per-plugin channel is args_json via InitEx).
  NvrGameContext ctx = {};
  ctx.base_addr = reinterpret_cast<uintptr_t>(EchoVR::g_GameBaseAddress);
  ctx.net_game = nullptr;
  ctx.game_state = 0;
  ctx.flags = 0;
  if (g_isServer) ctx.flags |= NEVR_HOST_IS_SERVER;
  else ctx.flags |= NEVR_HOST_IS_CLIENT;
  if (g_isHeadless) ctx.flags |= NEVR_HOST_IS_HEADLESS;

  for (const auto& item : plan) {
    const std::string path = pluginDir + item.file;
    const char* filename = item.file.c_str();

    // N89: refuse plugins whose function is now BUILT IN to gamepatches. Loading
    // one makes two MinHook instances (gamepatches links extern/minhook, plugins
    // link the vcpkg port — separate static copies, separate hook tables) fight
    // over the same target. This survives the cutover to config-driven loading:
    // an operator naming log_filter.dll explicitly is still an error we correct,
    // because the built-in filter already provides it. Refused even if the entry
    // is marked required — the function is present (built in), so it is not a
    // missing dependency; warn and continue rather than fatal.
    {
      static const char* kSupersededByBuiltin[] = {"log_filter.dll", "log-filter.dll"};
      bool superseded = false;
      for (const char* s : kSupersededByBuiltin) {
        if (_stricmp(filename, s) == 0) { superseded = true; break; }
      }
      if (superseded) {
        Log(EchoVR::LogLevel::Warning,
            "[NEVR.PLUGIN] SKIPPED %s — superseded by the built-in log filter. Loading it "
            "would install a second MinHook on CLog::PrintfImpl and silently disable both "
            "file logging and max_line_length truncation (N89). Remove it from config.yaml.",
            filename);
        continue;
      }
    }

    /* N75: LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32.
     *
     * The risk was never loading OUR dll — we pass a full path. It is how ITS
     * dependencies resolve: with dwFlags=0 the search order starts at the
     * application directory, so a dll dropped next to echovr.exe can satisfy a
     * dependency ahead of the real one. These flags restrict the search to the
     * loaded dll's own directory plus System32.
     *
     * Not hypothetical: N89 was a stale dll sitting in plugins/ silently taking
     * over the log filter for entire runs. That was an accident; the same directory
     * and the same loader are what an attacker would use deliberately.
     *
     * The flags require an absolute path (we have one). If the OS rejects them we
     * log and fall back rather than failing to load — but we say so, because a
     * silent fallback would defeat the whole point. */
    HMODULE hPlugin = LoadLibraryExA(path.c_str(), nullptr,
                                     LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR |
                                     LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!hPlugin && GetLastError() == ERROR_INVALID_PARAMETER) {
      Log(EchoVR::LogLevel::Warning,
          "[NEVR.PLUGIN] %s: restricted search flags unsupported — falling back to "
          "default search order (N75 hardening inactive for this load)", filename);
      hPlugin = LoadLibraryA(path.c_str());
    }
    if (!hPlugin) {
      FailPluginLoad(item, "LoadLibrary failed: error " + std::to_string(GetLastError()));
      continue;
    }

    auto getInfoFn = reinterpret_cast<NvrPluginGetInfo_fn>(GetProcAddress(hPlugin, "NvrPluginGetInfo"));
    if (!getInfoFn) {
      FreeLibrary(hPlugin);
      FailPluginLoad(item, "missing NvrPluginGetInfo export");
      continue;
    }

    NvrPluginInfo info = getInfoFn();
    if (!info.name) {
      FreeLibrary(hPlugin);
      FailPluginLoad(item, "NvrPluginGetInfo returned NULL name");
      continue;
    }

    // Check API version (v1 plugins lack this export). POLICY (unchanged, N134
    // S6): a plugin declaring a version ABOVE the host WARNS and loads anyway —
    // NOT a fatal, and NOT gated on `required`. The module loader fatals on an
    // unsupported version; the plugin loader deliberately does not, and this
    // asymmetry is a flagged owner decision (see the N134 ledger entry). `required`
    // governs load/init FAILURE, not version skew.
    auto apiVersionFn = reinterpret_cast<NvrPluginGetApiVersion_fn>(GetProcAddress(hPlugin, "NvrPluginGetApiVersion"));
    uint32_t apiVersion = apiVersionFn ? apiVersionFn() : 1;
    if (apiVersion > NEVR_PLUGIN_API_VERSION) {
      Log(EchoVR::LogLevel::Warning,
          "[NEVR.PLUGIN] %s requires API v%u, host supports v%u — loading anyway",
          filename, apiVersion, NEVR_PLUGIN_API_VERSION);
    }

    // What does this plugin DO? (API v3, optional export — absent => UNDECLARED.)
    // A declaration, not an enforcement: the host cannot verify these bits and a
    // plugin that lies is believed. It exists so honest plugins are legible, so a
    // server can require a declaration before accepting a session, and so an
    // operator can see the answer without reading the plugin's source.
    auto capsFn = reinterpret_cast<NvrPluginGetCapabilities_fn>(GetProcAddress(hPlugin, "NvrPluginGetCapabilities"));
    const uint32_t caps = capsFn ? capsFn() : NEVR_PLUGIN_CAP_UNDECLARED;

    // Resolve init exports: v4 args-aware NvrPluginInitEx (preferred) and v3
    // NvrPluginInit (fallback), plus the tick/state/shutdown lifecycle exports.
    auto initExFn = reinterpret_cast<NvrPluginInitEx_fn>(GetProcAddress(hPlugin, "NvrPluginInitEx"));
    auto initFn = reinterpret_cast<NvrPluginInit_fn>(GetProcAddress(hPlugin, "NvrPluginInit"));
    auto onFrameFn = reinterpret_cast<NvrPluginOnFrame_fn>(GetProcAddress(hPlugin, "NvrPluginOnFrame"));
    auto onStateChangeFn = reinterpret_cast<NvrPluginOnGameStateChange_fn>(GetProcAddress(hPlugin, "NvrPluginOnGameStateChange"));
    auto shutdownFn = reinterpret_cast<NvrPluginShutdown_fn>(GetProcAddress(hPlugin, "NvrPluginShutdown"));

    const PluginInitKind initKind =
        ChoosePluginInit(initExFn != nullptr, initFn != nullptr);
    const char* initVia =
        initKind == PluginInitKind::Ex ? "InitEx" :
        initKind == PluginInitKind::Legacy ? "Init" : "no-init";

    // A v3 plugin (only NvrPluginInit) has no args channel — if the operator
    // configured args for it, say so rather than dropping them silently. Do NOT
    // log the args_json itself (it may hold secrets); log only that args exist.
    if (initKind == PluginInitKind::Legacy && item.args_json != "{}") {
      Log(EchoVR::LogLevel::Warning,
          "[NEVR.PLUGIN] %s: args configured but the plugin exports only the v3 "
          "NvrPluginInit (no args channel) — args ignored", filename);
    }

    int result = 0;
    switch (initKind) {
      case PluginInitKind::Ex:     result = initExFn(&ctx, item.args_json.c_str()); break;
      case PluginInitKind::Legacy: result = initFn(&ctx); break;
      case PluginInitKind::None:   break;  // no init export — plugin still loads (init optional)
    }
    if (result != 0) {
      FreeLibrary(hPlugin);
      FailPluginLoad(item, std::string(initVia) + " returned code " + std::to_string(result));
      continue;
    }

    // N84: a plugin installs its hooks in init. Re-verify every address
    // gamepatches already detoured — if the bytes changed, this plugin hooked
    // an address we own. Detects third-party plugins, which no source-level
    // check can see: gamepatches and plugins link SEPARATE static MinHook
    // copies, so neither library can report the collision itself.
    //
    // N120: the return used to be DISCARDED. The collision was logged at ERROR
    // and the plugin loaded anyway, so the detection existed and changed
    // nothing. A non-zero count means a third party now owns an address we
    // detoured: OUR patch is silently not applied, and every conclusion drawn
    // from "the hook is installed" is wrong from here on.
    if (initKind != PluginInitKind::None) {
      const int collisions = HookGuard::VerifyAll(filename);
      if (collisions > 0) {
        ServerFatal("Plugin %s re-hooked %d address(es) this runtime already owns "
                    "— our patches at those addresses are no longer applied",
                    filename, collisions);
      }
    }

    g_plugins.push_back({hPlugin, info, apiVersion, caps, initFn, onFrameFn,
                         onStateChangeFn, shutdownFn, path});
    Log(EchoVR::LogLevel::Info, "[NEVR.PLUGIN] Loaded: %s v%u.%u.%u (API v%u) caps=0x%02X%s via %s",
        info.name,
        info.version_major, info.version_minor, info.version_patch,
        apiVersion, caps,
        caps == NEVR_PLUGIN_CAP_UNDECLARED ? " UNDECLARED" : "",
        initVia);
    if (caps == NEVR_PLUGIN_CAP_UNDECLARED) {
      // Not a warning about THIS plugin — a warning that we cannot answer the
      // question a server will ask. Info, because every pre-v3 plugin is here.
      Log(EchoVR::LogLevel::Info,
          "[NEVR.PLUGIN] %s declares no capabilities (pre-v3 or omitted). It is not "
          "known whether it affects gameplay; treat as unknown, not as harmless.",
          info.name);
    }
  }

  Log(EchoVR::LogLevel::Info, "[NEVR.PLUGIN] %zu plugin(s) loaded", g_plugins.size());
}

void UnloadPlugins() {
  for (auto it = g_plugins.rbegin(); it != g_plugins.rend(); ++it) {
    if (it->shutdown) {
      it->shutdown();
    }
    FreeLibrary(it->hModule);
  }
  g_plugins.clear();
}

void TickPlugins(const NvrGameContext* ctx) {
  for (auto& p : g_plugins) {
    if (p.on_frame) {
      p.on_frame(ctx);
    }
  }
}

void NotifyPluginsStateChange(const NvrGameContext* ctx, uint32_t old_state, uint32_t new_state) {
  for (auto& p : g_plugins) {
    if (p.on_state_change) {
      p.on_state_change(ctx, old_state, new_state);
    }
  }
}

// ============================================================================
// Test hooks — NEVR_TEST_HOOKS enables unit-test injection of mock callbacks
// into the static plugin registry, so behavioral tests can verify that
// TickPlugins / NotifyPluginsStateChange actually fire registered callbacks.
// These are NEVER compiled into production builds (gamepatches.dll).
// ============================================================================

#ifdef NEVR_TEST_HOOKS

void TestHook_RegisterPluginOnFrame(NvrPluginOnFrame_fn fn) {
  LoadedPlugin p = {};
  p.hModule = nullptr;
  p.on_frame = fn;
  p.on_state_change = nullptr;
  p.shutdown = nullptr;
  g_plugins.push_back(p);
}

void TestHook_RegisterPluginOnStateChange(NvrPluginOnGameStateChange_fn fn) {
  LoadedPlugin p = {};
  p.hModule = nullptr;
  p.on_frame = nullptr;
  p.on_state_change = fn;
  p.shutdown = nullptr;
  g_plugins.push_back(p);
}

void TestHook_ClearPlugins() {
  g_plugins.clear();
}

#endif  // NEVR_TEST_HOOKS
