// plugin_load_plan.cpp — see plugin_load_plan_build.h. PURE: nevr_config + std +
// nlohmann-json only. No singleton, no windows.h, no Log — so test_plugin_load_plan
// links it standalone (like service_map.cpp). Compiled SKIP_PRECOMPILE_HEADERS
// with NOMINMAX defined before anything, because nlohmann-json pulls <limits> and
// the core PCH's windows.h (reached transitively in the DLL build) defines the
// min/max macros that break it. Mirrors nevr_config.cpp / service_config.cpp.

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "runtime/ext/plugin_load_plan_build.h"

#include <nlohmann/json.hpp>

namespace nevr_plugincfg {

namespace {

// True when `text` is not valid UTF-8, i.e. nlohmann's strict dump() throws on it.
// Catches the NAMED type_error (error 316) only.
bool IsInvalidUtf8(const std::string& text) {
  try {
    (void)nlohmann::json(text).dump();
    return false;
  } catch (const nlohmann::json::type_error&) {
    return true;
  }
}

}  // namespace

std::string ArgsToJson(const std::map<std::string, std::string>& args,
                       std::vector<std::string>* replacedKeys) {
  // ordered_json would also work; a plain object over a std::map is already
  // deterministic (sorted keys). Every value is emitted as a JSON string — the
  // v4 contract is "flat object, string values" (post-interpolation scalars).
  nlohmann::json obj = nlohmann::json::object();
  for (const auto& kv : args) {
    obj[kv.first] = kv.second;
    // A value can hold bytes that are not UTF-8 (a ${VAR} resolved from the ANSI
    // environment, a mistyped config.yaml). Record which keys are affected so the
    // loader can say so; this layer stays free of logging.
    if (replacedKeys != nullptr && (IsInvalidUtf8(kv.first) || IsInvalidUtf8(kv.second))) {
      replacedKeys->push_back(kv.first);
    }
  }
  // Strict dump first; on invalid UTF-8 the default dump() throws type_error 316
  // -- at boot, where nothing catches it, so the client would die before any
  // plugin loads. Fall back to `replace` (as BuildPluginManifest does): each bad
  // byte becomes U+FFFD. "{}" for an empty map.
  try {
    return obj.dump();
  } catch (const nlohmann::json::type_error&) {
    return obj.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
  }
}

std::vector<PluginLoadItem> BuildLoadPlan(const nevr::NevrConfig& cfg) {
  std::vector<PluginLoadItem> plan;
  for (const nevr::PluginSpec& spec : cfg.Plugins()) {
    // enabled:false is carried, not dropped: the loader skips it, and the login
    // reports it as configured-but-disabled (#60).
    PluginLoadItem item;
    item.enabled = spec.enabled;
    item.name = spec.name;
    // The parser already defaulted file to name+".dll" when the entry omitted it
    // (nevr_config.cpp ParsePlugins), so spec.file is always non-empty here.
    item.file = spec.file;
    item.required = spec.required;
    item.target = spec.target;
    item.args_json = ArgsToJson(spec.args, &item.args_replaced_keys);
    plan.push_back(std::move(item));
  }
  return plan;
}

}  // namespace nevr_plugincfg
