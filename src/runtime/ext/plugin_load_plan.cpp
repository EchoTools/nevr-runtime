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

std::string ArgsToJson(const std::map<std::string, std::string>& args) {
  // ordered_json would also work; a plain object over a std::map is already
  // deterministic (sorted keys). Every value is emitted as a JSON string — the
  // v4 contract is "flat object, string values" (post-interpolation scalars).
  nlohmann::json obj = nlohmann::json::object();
  for (const auto& kv : args) {
    obj[kv.first] = kv.second;
  }
  return obj.dump();  // "{}" for an empty map
}

std::vector<PluginLoadItem> BuildLoadPlan(const nevr::NevrConfig& cfg) {
  std::vector<PluginLoadItem> plan;
  for (const nevr::PluginSpec& spec : cfg.Plugins()) {
    if (!spec.enabled) continue;  // enabled:false -> skip (still counts as configured)
    PluginLoadItem item;
    item.name = spec.name;
    // The parser already defaulted file to name+".dll" when the entry omitted it
    // (nevr_config.cpp ParsePlugins), so spec.file is always non-empty here.
    item.file = spec.file;
    item.required = spec.required;
    item.target = spec.target;
    item.args_json = ArgsToJson(spec.args);
    plan.push_back(std::move(item));
  }
  return plan;
}

}  // namespace nevr_plugincfg
