#include "runtime/ext/plugin_loader.h"

#include <string>
#include <vector>

#include "core/globals.h"
#include "core/logging.h"

#include "runtime/lifecycle/cli.h"
#include "runtime/hook/hook_guard.h"

struct LoadedPlugin {
  HMODULE                     hModule;
  NvrPluginInfo               info;
  NvrPluginInit_fn            init;
  NvrPluginOnFrame_fn         on_frame;
  NvrPluginOnGameStateChange_fn on_state_change;
  NvrPluginShutdown_fn        shutdown;
  std::string                 path;
};

static std::vector<LoadedPlugin> g_plugins;

void LoadPlugins() {
  // Resolve plugins/ directory relative to echovr.exe (same dir as pnsradgameserver.dll)
  CHAR moduleDir[MAX_PATH] = {0};
  GetModuleFileNameA((HMODULE)EchoVR::g_GameBaseAddress, moduleDir, MAX_PATH);
  CHAR* lastSlash = strrchr(moduleDir, '\\');
  if (lastSlash) *(lastSlash + 1) = '\0';

  std::string pluginDir = std::string(moduleDir) + "plugins\\";
  std::string searchPattern = pluginDir + "*.dll";

  Log(EchoVR::LogLevel::Info, "[NEVR.PLUGIN] Scanning for plugins in: %s", pluginDir.c_str());

  WIN32_FIND_DATAA findData;
  HANDLE hFind = FindFirstFileA(searchPattern.c_str(), &findData);
  if (hFind == INVALID_HANDLE_VALUE) {
    DWORD err = GetLastError();
    if (err == ERROR_FILE_NOT_FOUND || err == ERROR_PATH_NOT_FOUND) {
      Log(EchoVR::LogLevel::Info, "[NEVR.PLUGIN] No plugins directory or no plugins found");
    } else {
      Log(EchoVR::LogLevel::Warning, "[NEVR.PLUGIN] FindFirstFile failed: %lu", err);
    }
    return;
  }

  std::vector<std::string> dllPaths;
  do {
    if (!(findData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
      dllPaths.push_back(pluginDir + findData.cFileName);
    }
  } while (FindNextFileA(hFind, &findData));
  FindClose(hFind);

  if (dllPaths.empty()) {
    Log(EchoVR::LogLevel::Info, "[NEVR.PLUGIN] No plugin DLLs found");
    return;
  }

  Log(EchoVR::LogLevel::Info, "[NEVR.PLUGIN] Found %zu plugin candidate(s)", dllPaths.size());

  // Build init context
  NvrGameContext ctx = {};
  ctx.base_addr = (uintptr_t)EchoVR::g_GameBaseAddress;
  ctx.net_game = nullptr;
  ctx.game_state = 0;
  ctx.flags = 0;
  if (g_isServer) ctx.flags |= NEVR_HOST_IS_SERVER;
  else ctx.flags |= NEVR_HOST_IS_CLIENT;
  if (g_isHeadless) ctx.flags |= NEVR_HOST_IS_HEADLESS;

  for (const auto& path : dllPaths) {
    // Extract filename for logging
    const char* filename = strrchr(path.c_str(), '\\');
    filename = filename ? filename + 1 : path.c_str();

    // N89: skip plugins whose function is now BUILT IN to gamepatches. Loading
    // one makes two MinHook instances (gamepatches links extern/minhook, plugins
    // link the vcpkg port — separate static copies, separate hook tables) fight
    // over the same target.
    //
    // Measured with log_filter.dll deployed: the builtin filter's JSONL contains
    // only [NEVR.*] lines and stops exactly at plugin load — ZERO game log lines
    // for the whole run — and max_line_length stopped applying, so 8 KB
    // "[NSUSER] saved <path>: <8KB JSON>" profile dumps reached the console
    // untruncated (30.5% of one server log). The limit was never removed; the
    // second hook broke the chain that applied it.
    //
    // CLAUDE.md already records log-filter as built in. A stale deployed copy is
    // therefore not a plugin the operator chose — it is a leftover.
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
            "file logging and max_line_length truncation (N89). Delete it from plugins/.",
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
      Log(EchoVR::LogLevel::Warning, "[NEVR.PLUGIN] Failed to load %s: error %lu", filename, GetLastError());
      continue;
    }

    auto getInfoFn = (NvrPluginGetInfo_fn)GetProcAddress(hPlugin, "NvrPluginGetInfo");
    if (!getInfoFn) {
      Log(EchoVR::LogLevel::Warning, "[NEVR.PLUGIN] %s: missing NvrPluginGetInfo export, skipping", filename);
      FreeLibrary(hPlugin);
      continue;
    }

    NvrPluginInfo info = getInfoFn();
    if (!info.name) {
      Log(EchoVR::LogLevel::Warning, "[NEVR.PLUGIN] %s: NvrPluginGetInfo returned NULL name, skipping", filename);
      FreeLibrary(hPlugin);
      continue;
    }

    // Check API version (v1 plugins lack this export)
    auto apiVersionFn = (NvrPluginGetApiVersion_fn)GetProcAddress(hPlugin, "NvrPluginGetApiVersion");
    uint32_t apiVersion = apiVersionFn ? apiVersionFn() : 1;
    if (apiVersion > NEVR_PLUGIN_API_VERSION) {
      Log(EchoVR::LogLevel::Warning,
          "[NEVR.PLUGIN] %s requires API v%u, host supports v%u — loading anyway",
          filename, apiVersion, NEVR_PLUGIN_API_VERSION);
    }

    // Resolve optional exports
    auto initFn = (NvrPluginInit_fn)GetProcAddress(hPlugin, "NvrPluginInit");
    auto onFrameFn = (NvrPluginOnFrame_fn)GetProcAddress(hPlugin, "NvrPluginOnFrame");
    auto onStateChangeFn = (NvrPluginOnGameStateChange_fn)GetProcAddress(hPlugin, "NvrPluginOnGameStateChange");
    auto shutdownFn = (NvrPluginShutdown_fn)GetProcAddress(hPlugin, "NvrPluginShutdown");

    // Call init if present
    if (initFn) {
      int result = initFn(&ctx);
      if (result != 0) {
        Log(EchoVR::LogLevel::Warning, "[NEVR.PLUGIN] %s (%s): init failed with code %d, unloading",
            info.name, info.description ? info.description : "?", result);
        FreeLibrary(hPlugin);
        continue;
      }

      // N84: a plugin installs its hooks in init. Re-verify every address
      // gamepatches already detoured — if the bytes changed, this plugin hooked
      // an address we own. Detects third-party plugins, which no source-level
      // check can see: gamepatches and plugins link SEPARATE static MinHook
      // copies, so neither library can report the collision itself.
      HookGuard::VerifyAll(filename);
    }

    g_plugins.push_back({hPlugin, info, initFn, onFrameFn, onStateChangeFn, shutdownFn, path});
    Log(EchoVR::LogLevel::Info, "[NEVR.PLUGIN] Loaded: %s v%u.%u.%u (API v%u)",
        info.name,
        info.version_major, info.version_minor, info.version_patch,
        apiVersion);
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
