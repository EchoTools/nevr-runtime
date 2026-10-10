// plugin_manifest.h — the plugin report the client login carries as `nevr_plugins` (#60).
//
// PCH-safe on purpose: std types only, like plugin_load_plan.h. The loader
// (plugin_loader.cpp) is compiled with the core PCH, whose windows.h defines the
// min/max macros nlohmann-json trips over, so the JSON half lives in
// plugin_manifest.cpp (SKIP_PRECOMPILE_HEADERS + NOMINMAX) and is pure: no
// windows.h, no Log, no globals. The test links it standalone.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

// What happened to one entry of config.yaml's `plugins:` list. The loader fills
// one of these per configured entry, in list order, including disabled ones.
struct PluginManifestEntry {
  std::string name;  // the config.yaml entry's `name`
  std::string file;  // the dll filename the loader looks for in plugins/
  bool enabled = true;
  bool required = false;
  bool loaded = false;
  // Why an enabled plugin did not load: the same clause FailPluginLoad logs.
  // Never carries plugin args. Empty when the plugin loaded or is disabled.
  std::string error;
  // Filled only when loaded: the plugin's own NvrPluginGetInfo version, its API
  // version, and its NvrPluginCapabilities bitmask (0 = undeclared).
  uint32_t version_major = 0;
  uint32_t version_minor = 0;
  uint32_t version_patch = 0;
  uint32_t api_version = 0;
  uint32_t capabilities = 0;
};

// Serialize the entries to the `nevr_plugins` JSON array. One object per entry,
// in the given order:
//   {"name","file","enabled","required","loaded"}       always
//   "error"                                             when enabled and not loaded
//   "ver" ("M.m.p"), "api", "caps"                      when loaded
// Returns "[]" for no entries. Invalid UTF-8 in a name or file is replaced with
// U+FFFD rather than thrown, so the output always parses.
std::string BuildPluginManifest(const std::vector<PluginManifestEntry>& entries);
