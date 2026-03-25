#include "plugin_loader.h"

#include <string>
#include <vector>

#include "common/globals.h"
#include "common/logging.h"
#include "common/plugin_api.h"

// Defined in patches.cpp (gamepatches-internal, not in globals.h)
extern BOOL g_isServer;

struct LoadedPlugin {
  HMODULE                hModule;
  NevRPluginInfo*        info;
  NevRPluginDestroyFunc  destroy;  // may be NULL
  std::string            path;
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

  NevRPluginContext ctx = {};
  ctx.api_version = NEVR_PLUGIN_API_VERSION;
  ctx.nevr_version = PROJECT_VERSION;
  ctx.game_base = (uintptr_t)EchoVR::g_GameBaseAddress;
  ctx.is_server = g_isServer != FALSE;
  ctx.is_headless = g_isHeadless != FALSE;

  for (const auto& path : dllPaths) {
    // Extract filename for logging
    const char* filename = strrchr(path.c_str(), '\\');
    filename = filename ? filename + 1 : path.c_str();

    HMODULE hPlugin = LoadLibraryA(path.c_str());
    if (!hPlugin) {
      Log(EchoVR::LogLevel::Warning, "[NEVR.PLUGIN] Failed to load %s: error %lu", filename, GetLastError());
      continue;
    }

    auto createFn = (NevRPluginCreateFunc)GetProcAddress(hPlugin, "NevRPluginCreate");
    if (!createFn) {
      Log(EchoVR::LogLevel::Warning, "[NEVR.PLUGIN] %s: missing NevRPluginCreate export, skipping", filename);
      FreeLibrary(hPlugin);
      continue;
    }

    NevRPluginInfo* info = createFn();
    if (!info) {
      Log(EchoVR::LogLevel::Warning, "[NEVR.PLUGIN] %s: NevRPluginCreate returned NULL, skipping", filename);
      FreeLibrary(hPlugin);
      continue;
    }

    if (info->api_version != NEVR_PLUGIN_API_VERSION) {
      Log(EchoVR::LogLevel::Warning, "[NEVR.PLUGIN] %s: API version mismatch (plugin=%u, host=%u), skipping",
          filename, info->api_version, NEVR_PLUGIN_API_VERSION);
      FreeLibrary(hPlugin);
      continue;
    }

    // Check mode filter
    NevRPluginMode mode = info->mode;
    if (mode == 0) mode = NEVR_PLUGIN_MODE_BOTH;  // default if unset

    bool wantServer = (mode & NEVR_PLUGIN_MODE_SERVER) != 0;
    bool wantClient = (mode & NEVR_PLUGIN_MODE_CLIENT) != 0;

    if (g_isServer && !wantServer) {
      Log(EchoVR::LogLevel::Info, "[NEVR.PLUGIN] %s (%s): skipped (client-only plugin, running as server)",
          info->name ? info->name : filename, info->version ? info->version : "?");
      FreeLibrary(hPlugin);
      continue;
    }
    if (!g_isServer && !wantClient) {
      Log(EchoVR::LogLevel::Info, "[NEVR.PLUGIN] %s (%s): skipped (server-only plugin, running as client)",
          info->name ? info->name : filename, info->version ? info->version : "?");
      FreeLibrary(hPlugin);
      continue;
    }

    // Call on_load
    if (info->on_load) {
      int result = info->on_load(&ctx);
      if (result != 0) {
        Log(EchoVR::LogLevel::Warning, "[NEVR.PLUGIN] %s (%s): on_load failed with code %d, unloading",
            info->name ? info->name : filename, info->version ? info->version : "?", result);
        FreeLibrary(hPlugin);
        continue;
      }
    }

    auto destroyFn = (NevRPluginDestroyFunc)GetProcAddress(hPlugin, "NevRPluginDestroy");

    g_plugins.push_back({hPlugin, info, destroyFn, path});
    Log(EchoVR::LogLevel::Info, "[NEVR.PLUGIN] Loaded: %s v%s [%s]",
        info->name ? info->name : filename,
        info->version ? info->version : "?",
        (mode == NEVR_PLUGIN_MODE_SERVER) ? "server" :
        (mode == NEVR_PLUGIN_MODE_CLIENT) ? "client" : "both");
  }

  Log(EchoVR::LogLevel::Info, "[NEVR.PLUGIN] %zu plugin(s) loaded", g_plugins.size());
}

void UnloadPlugins() {
  for (auto it = g_plugins.rbegin(); it != g_plugins.rend(); ++it) {
    if (it->info && it->info->on_unload) {
      it->info->on_unload();
    }
    if (it->destroy && it->info) {
      it->destroy(it->info);
    }
    FreeLibrary(it->hModule);
  }
  g_plugins.clear();
}
