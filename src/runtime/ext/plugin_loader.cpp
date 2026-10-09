#include "runtime/ext/plugin_loader.h"

#include <algorithm>
#include <mutex>
#include <string>
#include <vector>

#include "core/globals.h"
#include "core/logging.h"

#include "runtime/lifecycle/cli.h"
#include "runtime/lifecycle/crash_recovery.h"  // ServerFatal
#include "runtime/hook/hook_guard.h"
#include "runtime/ext/plugin_load_plan.h"       // N134 S6: config-driven load plan
#include "runtime/ext/plugin_manifest.h"        // #60: the login's plugin report

struct LoadedPlugin {
  HMODULE                     hModule;
  NvrPluginInfo               info;
  uint32_t                    api_version;
  uint32_t                    capabilities;  /* NvrPluginCapabilities bitmask; 0 = UNDECLARED */
  /* What get_plugin_info hands out: its own struct, not a cast of `info` (whose
   * padding sits where NvrLoadedPluginInfo has api_version). */
  NvrLoadedPluginInfo         loaded_info;
  NvrPluginInit_fn            init;
  NvrPluginOnFrame_fn         on_frame;
  NvrPluginOnGameStateChange_fn on_state_change;
  NvrPluginShutdown_fn        shutdown;
  std::string                 path;
};

static std::vector<LoadedPlugin> g_plugins;

// #60: what happened to each configured plugin at the last LoadPlugins(), in
// config.yaml order. Written once at the end of LoadPlugins (boot thread), read
// by the login builder on the bridge's websocket thread, hence the mutex.
static std::mutex g_pluginReportMutex;
static std::vector<PluginManifestEntry> g_pluginReport;

static void PublishPluginReport(std::vector<PluginManifestEntry> report) {
  std::lock_guard<std::mutex> lock(g_pluginReportMutex);
  g_pluginReport = std::move(report);
}

// N134 S8: query API — lets a plugin discover its neighbours at runtime.
// Called through ctx->get_plugin_count / ctx->get_plugin_info (function
// pointers filled into NvrGameContext by LoadPlugins and every per-frame/
// per-state-change ctx construction). Returns the count of successfully
// loaded plugins; a plugin that failed init or was refused (N89) is not in
// g_plugins and therefore not counted.

int GetLoadedPluginCount(void) {
  return static_cast<int>(g_plugins.size());
}

const NvrLoadedPluginInfo* GetLoadedPluginInfo(int index) {
  if (index < 0 || static_cast<size_t>(index) >= g_plugins.size()) return nullptr;
  // The plugin's own NvrLoadedPluginInfo. Not a cast of p.info: NvrPluginInfo is
  // padded to 32 bytes, so through it api_version read the padding and
  // capabilities read api_version. Stable for the process lifetime: LoadPlugins
  // reserves g_plugins before any init, so it never reallocates under a plugin
  // that kept the pointer (and it never shrinks after load).
  return &g_plugins[index].loaded_info;
}

// N112 / #60 — the login's `nevr_plugins` array: one entry per configured
// plugin (loaded, failed, or disabled). Called from ws_bridge.cpp's login
// builder. The JSON is built by the pure BuildPluginManifest (nlohmann-json).
std::string BuildPluginManifestJson() {
  std::lock_guard<std::mutex> lock(g_pluginReportMutex);
  return BuildPluginManifest(g_pluginReport);
}


// N134 S6: required-aware load failure. A plugin the config declared
// `required: true` that fails to load or init is FATAL on a dedicated server
// (ServerFatal -> ForceFatalExit; the same N120 rule modules follow) and a hard
// warning on a client (ServerFatal warns and returns in client mode). An optional
// plugin (`required: false`, the default) warns and the loader continues. Caller
// `continue`s after this returns. `reason` is a fully-formed clause (no values
// that could be secrets — plugin args are NOT interpolated into it). The same
// clause goes into the login's plugin report (#60) as that entry's `error`.
static void FailPluginLoad(const PluginLoadItem& item, const std::string& reason,
                           PluginManifestEntry& report) {
  report.loaded = false;
  report.error = reason;
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
  std::vector<PluginLoadItem> plan = NevrCfgPluginLoadPlan();
  if (plan.empty()) {
    PublishPluginReport({});
    Log(EchoVR::LogLevel::Info,
        "[NEVR.PLUGIN] no plugins configured in config.yaml — loading none");
    return;
  }

  // #60: one report entry per configured plugin, in config order. Each starts
  // "not loaded, no error" and is filled in as the loader decides its fate.
  std::vector<PluginManifestEntry> report(plan.size());
  size_t enabledCount = 0;
  for (size_t i = 0; i < plan.size(); ++i) {
    report[i].name = plan[i].name;
    report[i].file = plan[i].file;
    report[i].enabled = plan[i].enabled;
    report[i].required = plan[i].required;
    if (plan[i].enabled) ++enabledCount;
  }

  Log(EchoVR::LogLevel::Info,
      "[NEVR.PLUGIN] %zu plugin(s) configured, %zu enabled; loading in list order from %s",
      plan.size(), enabledCount, pluginDir.c_str());

  // Build init context (shared; the per-plugin channel is args_json via InitEx).
  NvrGameContext ctx = {};
  ctx.base_addr = reinterpret_cast<uintptr_t>(EchoVR::g_GameBaseAddress);
  ctx.net_game = nullptr;
  ctx.game_state = 0;
  ctx.flags = 0;
  if (g_isServer) ctx.flags |= NEVR_HOST_IS_SERVER;
  else ctx.flags |= NEVR_HOST_IS_CLIENT;
  if (g_isHeadless) ctx.flags |= NEVR_HOST_IS_HEADLESS;
  ctx.ctx_size = sizeof(NvrGameContext);
  ctx.get_plugin_count = GetLoadedPluginCount;
  ctx.get_plugin_info = GetLoadedPluginInfo;

  // --- Pass 1: LoadLibrary + resolve exports for every plugin, collecting into
  // a staging list. Init is deferred to pass 2 so we can stable-sort by
  // capability priority (N134 S8): observers before modifiers, modifiers before
  // engine-hookers. Within a priority band the config.yaml order is preserved.
  struct StagedPlugin {
    HMODULE                  hModule;
    PluginLoadItem            item;
    NvrPluginInfo             info;
    uint32_t                  apiVersion;
    uint32_t                  caps;      // NvrPluginCapabilities, for pass-2 sorting
    NvrPluginInitEx_fn        initExFn;
    NvrPluginInit_fn          initFn;
    NvrPluginOnFrame_fn       onFrameFn;
    NvrPluginOnGameStateChange_fn onStateChangeFn;
    NvrPluginShutdown_fn      shutdownFn;
    PluginInitKind            initKind;
    std::string               path;       // full path to DLL on disk
    size_t                    planIndex;  // #60: this plugin's entry in `report`
  };
  std::vector<StagedPlugin> staged;

  // A repeat of an entry is skipped (see below), but if the repeat is `required`
  // the plugin it names must still be fatal to lose: the requirement moves onto the
  // entry that is actually loaded. When that entry already failed non-fatally
  // (it was optional then), the failure is raised now, as fatal. Returns true when
  // the requirement was carried over.
  //
  // A skipped entry is recorded in `heldBy` against the entry that actually holds its
  // module, and `carryRequired` follows that chain, so a repeat of a skipped entry
  // reaches the entry that holds the module rather than the skipped one.
  std::vector<size_t> heldBy(plan.size());
  for (size_t i = 0; i < heldBy.size(); ++i) heldBy[i] = i;
  auto resolveHolder = [&heldBy](size_t index) -> size_t {
    while (heldBy[index] != index) index = heldBy[index];
    return index;
  };
  auto carryRequired = [&](size_t namedIndex, const PluginLoadItem& repeat) -> bool {
    const size_t firstIndex = resolveHolder(namedIndex);
    if (!repeat.required || plan[firstIndex].required) return false;
    plan[firstIndex].required = true;
    report[firstIndex].required = true;
    for (StagedPlugin& s : staged) {
      if (s.planIndex == firstIndex) s.item.required = true;
    }
    const PluginManifestEntry& first = report[firstIndex];
    if (!first.loaded && !first.error.empty()) {
      const std::string reason = first.error;
      FailPluginLoad(plan[firstIndex], reason, report[firstIndex]);
    }
    return true;
  };

  // Caps-priority sort predicate: lower `priority` loads first. Within a
  // single priority band the stable_sort preserves the config.yaml order.
  auto capsOrder = [](const StagedPlugin& a, const StagedPlugin& b) -> bool {
    return CapsLoadPriority(a.caps) < CapsLoadPriority(b.caps);
  };

  for (size_t planIndex = 0; planIndex < plan.size(); ++planIndex) {
    const PluginLoadItem& item = plan[planIndex];
    PluginManifestEntry& itemReport = report[planIndex];
    const std::string path = pluginDir + item.file;
    const char* filename = item.file.c_str();

    if (!item.enabled) {
      Log(EchoVR::LogLevel::Info,
          "[NEVR.PLUGIN] %s (%s): disabled in config.yaml — not loading",
          item.name.c_str(), filename);
      continue;
    }

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
            "file logging and max_line_length truncation. Remove it from config.yaml.",
            filename);
        itemReport.error = "superseded by the built-in log filter";
        continue;
      }
    }

    // The same file listed twice: LoadLibrary would return the module already
    // loaded, and its init would run again with every callback doubled. Refused
    // even if this entry is marked required: the plugin itself is loaded, and the
    // requirement is carried onto the earlier entry.
    {
      const long first = DuplicatePluginEntry(plan, planIndex);
      if (first >= 0) {
        const bool carried = carryRequired(static_cast<size_t>(first), item);
        heldBy[planIndex] = resolveHolder(static_cast<size_t>(first));
        const std::string carriedNote = carried
            ? " This entry is required, so entry " + std::to_string(first + 1) +
                  " is now required too."
            : std::string();
        Log(EchoVR::LogLevel::Warning,
            "[NEVR.PLUGIN] SKIPPED %s (%s) — the same file as entry %ld (%s), listed earlier "
            "in config.yaml. Loading it again would run its init twice and double every "
            "callback. This entry's args were ignored; entry %ld's args apply.%s "
            "Remove one of the entries.",
            item.name.c_str(), filename, first + 1, plan[first].name.c_str(), first + 1,
            carriedNote.c_str());
        itemReport.error = "listed twice in config.yaml";
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
          "default search order (hardening inactive for this load)", filename);
      hPlugin = LoadLibraryA(path.c_str());
    }
    if (!hPlugin) {
      FailPluginLoad(item, "LoadLibrary failed: error " + std::to_string(GetLastError()), itemReport);
      continue;
    }
    // The backstop for what the file-name check can't see: another spelling of the
    // same path. LoadLibrary counted a reference for this call; give it back.
    {
      StagedPlugin* already = nullptr;
      for (StagedPlugin& s : staged) {
        if (s.hModule == hPlugin) { already = &s; break; }
      }
      if (already) {
        FreeLibrary(hPlugin);
        const bool carried = carryRequired(already->planIndex, item);
        heldBy[planIndex] = resolveHolder(already->planIndex);
        const std::string carriedNote = carried
            ? " This entry is required, so " + already->item.name + " is now required too."
            : std::string();
        Log(EchoVR::LogLevel::Warning,
            "[NEVR.PLUGIN] SKIPPED %s (%s) — the same module as %s (%s), already loaded. "
            "This entry's args were ignored; %s's args apply.%s "
            "Remove one of the entries from config.yaml.",
            item.name.c_str(), filename, already->item.name.c_str(), already->item.file.c_str(),
            already->item.name.c_str(), carriedNote.c_str());
        itemReport.error = "the same module as " + already->item.name;
        continue;
      }
    }

    auto getInfoFn = reinterpret_cast<NvrPluginGetInfo_fn>(GetProcAddress(hPlugin, "NvrPluginGetInfo"));
    if (!getInfoFn) {
      FreeLibrary(hPlugin);
      FailPluginLoad(item, "missing NvrPluginGetInfo export", itemReport);
      continue;
    }

    NvrPluginInfo info = getInfoFn();
    if (!info.name) {
      FreeLibrary(hPlugin);
      FailPluginLoad(item, "NvrPluginGetInfo returned NULL name", itemReport);
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

    // Invalid UTF-8 in an arg (a ${VAR} from the ANSI environment, a mistyped
    // config.yaml) was serialized as U+FFFD. Name the plugin and the arg KEYS,
    // never the values (they may hold secrets).
    for (const std::string& key : item.args_replaced_keys) {
      Log(EchoVR::LogLevel::Warning,
          "[NEVR.PLUGIN] %s: arg '%s' held invalid UTF-8; bytes replaced with "
          "U+FFFD in args_json", item.name.c_str(), key.c_str());
    }

    // A v3 plugin (only NvrPluginInit) has no args channel — if the operator
    // configured args for it, say so rather than dropping them silently. Do NOT
    // log the args_json itself (it may hold secrets); log only that args exist.
    if (initKind == PluginInitKind::Legacy && item.args_json != "{}") {
      Log(EchoVR::LogLevel::Warning,
          "[NEVR.PLUGIN] %s: args configured but the plugin exports only the v3 "
          "NvrPluginInit (no args channel) — args ignored", filename);
    }

    // Stage for pass-2 caps-ordered init (N134 S8).
    staged.push_back({hPlugin, item, info, apiVersion, caps,
                      initExFn, initFn, onFrameFn, onStateChangeFn, shutdownFn,
                      initKind, path, planIndex});

  } // for (planIndex over plan)

  // Pass 2: stable-sort by capability priority (config order preserved within
  // a band), then init each plugin in that order.
  std::stable_sort(staged.begin(), staged.end(), capsOrder);

  // Log the sorted order so an operator can see the load sequence: one INFO
  // summary naming the order, with the full per-item detail at DEBUG — an
  // INFO per-item line would near-duplicate the
  // per-plugin "Loaded: ..." INFO confirmation below once each plugin's init
  // succeeds (Rule 12: INFO is summary, DEBUG is narrative).
  {
    std::string order;
    for (size_t i = 0; i < staged.size(); i++) {
      if (i > 0) order += ", ";
      order += staged[i].item.name;
    }
    Log(EchoVR::LogLevel::Info,
        "[NEVR.PLUGIN] load order (priority-sorted): %s", order.c_str());
  }
  for (size_t i = 0; i < staged.size(); i++) {
    const StagedPlugin& s = staged[i];
    const char* initVia = s.initKind == PluginInitKind::Ex ? "InitEx" :
                          s.initKind == PluginInitKind::Legacy ? "Init" : "no-init";
    Log(EchoVR::LogLevel::Debug,
        "[NEVR.PLUGIN] [%zu/%zu] %s v%u.%u.%u (API v%u) caps=0x%02X via %s (priority=%d)",
        i + 1, staged.size(),
        s.item.name.c_str(),
        s.info.version_major, s.info.version_minor, s.info.version_patch,
        s.apiVersion, s.caps, initVia, CapsLoadPriority(s.caps));
  }

  // A plugin may keep what get_plugin_info returned from inside its init: no
  // push_back below may move the ones already loaded.
  g_plugins.reserve(g_plugins.size() + staged.size());
  for (const StagedPlugin& s : staged) {
    const char* initVia = s.initKind == PluginInitKind::Ex ? "InitEx" :
                          s.initKind == PluginInitKind::Legacy ? "Init" : "no-init";
    int result = 0;
    switch (s.initKind) {
      case PluginInitKind::Ex:     result = s.initExFn(&ctx, s.item.args_json.c_str()); break;
      case PluginInitKind::Legacy: result = s.initFn(&ctx); break;
      case PluginInitKind::None:   break;  // no init export — plugin still loads
    }
    if (result != 0) {
      FreeLibrary(s.hModule);
      FailPluginLoad(s.item, std::string(initVia) + " returned code " + std::to_string(result),
                     report[s.planIndex]);
      continue;
    }

    // N84: re-verify hooked addresses after this plugin's init may have
    // installed its own detours (see the full comment in the original; moved
    // here as the guard is unchanged from S6 except the variables are now
    // on the staged struct).
    if (s.initKind != PluginInitKind::None) {
      const int collisions = HookGuard::VerifyAll(s.item.file.c_str());
      if (collisions > 0) {
        ServerFatal("Plugin %s re-hooked %d address(es) this runtime already owns "
                    "— our patches at those addresses are no longer applied",
                    s.item.file.c_str(), collisions);
      }
    }

    const NvrLoadedPluginInfo loadedInfo = {
        s.info.name, s.info.description,
        s.info.version_major, s.info.version_minor, s.info.version_patch,
        s.apiVersion, s.caps};
    g_plugins.push_back({s.hModule, s.info, s.apiVersion, s.caps, loadedInfo,
                         s.initFn, s.onFrameFn,
                         s.onStateChangeFn, s.shutdownFn, s.path});
    {
      PluginManifestEntry& r = report[s.planIndex];
      r.loaded = true;
      r.error.clear();
      r.version_major = s.info.version_major;
      r.version_minor = s.info.version_minor;
      r.version_patch = s.info.version_patch;
      r.api_version = s.apiVersion;
      r.capabilities = s.caps;
    }
    Log(EchoVR::LogLevel::Info,
        "[NEVR.PLUGIN] Loaded: %s v%u.%u.%u (API v%u) caps=0x%02X%s via %s",
        s.info.name,
        s.info.version_major, s.info.version_minor, s.info.version_patch,
        s.apiVersion, s.caps,
        s.caps == NEVR_PLUGIN_CAP_UNDECLARED ? " UNDECLARED" : "",
        initVia);
    if (s.caps == NEVR_PLUGIN_CAP_UNDECLARED) {
      Log(EchoVR::LogLevel::Info,
          "[NEVR.PLUGIN] %s declares no capabilities (pre-v3 or omitted). It is not "
          "known whether it affects gameplay; treat as unknown, not as harmless.",
          s.info.name);
    }
  }

  // Denominator (enabled entries) lets an operator tell "5 enabled, 5 loaded"
  // (healthy) from "5 enabled, 3 loaded" (2 silently missing) without
  // scrolling back through Warning lines to count. Disabled entries are not
  // missing, so they are not in the denominator.
  Log(EchoVR::LogLevel::Info, "[NEVR.PLUGIN] plugin load complete: %zu/%zu loaded",
      g_plugins.size(), enabledCount);

  PublishPluginReport(std::move(report));
}

void UnloadPlugins() {
  for (auto it = g_plugins.rbegin(); it != g_plugins.rend(); ++it) {
    if (it->shutdown) {
      it->shutdown();
    }
    FreeLibrary(it->hModule);
  }
  // Release the storage as well: the next load starts from capacity 0, so it is the reserve in
  // LoadPlugins (not leftover capacity from an earlier load) that keeps kept info pointers valid.
  std::vector<LoadedPlugin>().swap(g_plugins);
  PublishPluginReport({});  // nothing is loaded any more; do not report stale state
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
  PublishPluginReport({});
}

#endif  // NEVR_TEST_HOOKS
