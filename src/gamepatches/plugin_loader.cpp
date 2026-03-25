#include "plugin_loader.h"

#include <string>
#include <vector>

#include "common/globals.h"
#include "common/logging.h"

// Defined in patches.cpp (gamepatches-internal, not in globals.h)
extern BOOL g_isServer;
extern BOOL g_isHeadless;

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

  for (const auto& path : dllPaths) {
    // Extract filename for logging
    const char* filename = strrchr(path.c_str(), '\\');
    filename = filename ? filename + 1 : path.c_str();

    HMODULE hPlugin = LoadLibraryA(path.c_str());
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
    }

    g_plugins.push_back({hPlugin, info, initFn, onFrameFn, onStateChangeFn, shutdownFn, path});
    Log(EchoVR::LogLevel::Info, "[NEVR.PLUGIN] Loaded: %s v%u.%u.%u",
        info.name,
        info.version_major, info.version_minor, info.version_patch);
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
