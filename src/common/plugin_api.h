#pragma once

// NEVR Plugin API
//
// Plugins are DLLs placed in the plugins/ subdirectory next to echovr.exe.
// Each plugin must export a C function:
//
//   extern "C" __declspec(dllexport) NevRPluginInfo* NevRPluginCreate();
//
// The returned NevRPluginInfo is owned by the plugin and must remain valid
// until NevRPluginDestroy() is called (or process exit).
//
// Plugins can optionally export:
//
//   extern "C" __declspec(dllexport) void NevRPluginDestroy(NevRPluginInfo* info);

#include <cstdint>
#include <windows.h>

// Plugin mode flags — indicate when this plugin should be loaded.
// A plugin that sets neither flag is loaded in both modes.
enum NevRPluginMode : uint32_t {
  NEVR_PLUGIN_MODE_SERVER = 0x1,  // Load only when running as dedicated server
  NEVR_PLUGIN_MODE_CLIENT = 0x2,  // Load only when running as client
  NEVR_PLUGIN_MODE_BOTH   = 0x3,  // Load in both modes (default)
};

// Context passed to plugin callbacks. Provides everything a plugin needs
// to interact with the game and the NEVR runtime.
struct NevRPluginContext {
  uint32_t    api_version;      // NEVR_PLUGIN_API_VERSION
  const char* nevr_version;     // NEVR runtime version string
  uintptr_t   game_base;        // echovr.exe base address
  bool        is_server;        // true if running as dedicated server
  bool        is_headless;      // true if running in headless mode
  uint32_t    reserved[8];      // Future expansion (zero-initialized)
};

// Plugin info — returned by NevRPluginCreate().
struct NevRPluginInfo {
  uint32_t        api_version;  // Must be NEVR_PLUGIN_API_VERSION
  const char*     name;         // Human-readable plugin name
  const char*     version;      // Plugin version string
  NevRPluginMode  mode;         // When to load this plugin

  // Called after all plugins are discovered and the game has been patched.
  // ctx is valid for the lifetime of the plugin.
  // Return 0 on success, non-zero to indicate load failure (plugin will be unloaded).
  int (*on_load)(const NevRPluginContext* ctx);

  // Called on process detach (optional, may be NULL).
  void (*on_unload)();
};

// Current API version. Bump when NevRPluginInfo or NevRPluginContext changes.
#define NEVR_PLUGIN_API_VERSION 1

// Plugin DLL export signatures
typedef NevRPluginInfo* (*NevRPluginCreateFunc)();
typedef void (*NevRPluginDestroyFunc)(NevRPluginInfo* info);
