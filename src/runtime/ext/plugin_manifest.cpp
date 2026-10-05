// plugin_manifest.cpp — see plugin_manifest.h. PURE: std + nlohmann-json only.
// Compiled SKIP_PRECOMPILE_HEADERS with NOMINMAX defined first, because
// nlohmann-json pulls <limits> and the core PCH's windows.h defines min/max.
// Same arrangement as plugin_load_plan.cpp.

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "runtime/ext/plugin_manifest.h"

#include <nlohmann/json.hpp>

std::string BuildPluginManifest(const std::vector<PluginManifestEntry>& entries) {
  nlohmann::json out = nlohmann::json::array();
  for (const PluginManifestEntry& e : entries) {
    nlohmann::json p;
    p["name"] = e.name;
    p["file"] = e.file;
    p["enabled"] = e.enabled;
    p["required"] = e.required;
    p["loaded"] = e.loaded;
    if (e.loaded) {
      p["ver"] = std::to_string(e.version_major) + "." + std::to_string(e.version_minor) + "." +
                 std::to_string(e.version_patch);
      p["api"] = e.api_version;
      p["caps"] = e.capabilities;
    } else if (e.enabled) {
      p["error"] = e.error;
    }
    out.push_back(std::move(p));
  }
  // `replace`: names come from config.yaml, and dump() throws type_error 316 on
  // invalid UTF-8 by default. The login must not fail over a mistyped plugin name.
  return out.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
}
