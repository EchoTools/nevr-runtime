// plugin_load_plan_build.h — the yaml-facing PURE builder for the plugin load
// plan (N134 S6). Separate from plugin_load_plan.h because this reaches
// nevr_config.h (nevr::PluginSpec / NevrConfig), which pulls <optional>/<map> and
// therefore may only be included from a SKIP_PRECOMPILE_HEADERS translation unit
// that has defined NOMINMAX (plugin_load_plan.cpp, service_config.cpp, and the
// test). The loader never includes this — it takes the finished plan through
// plugin_load_plan.h's NevrCfgPluginLoadPlan().

#pragma once

#include <map>
#include <string>

#include "core/nevr_config.h"
#include "runtime/ext/plugin_load_plan.h"

namespace nevr_plugincfg {

// Serialize a plugin entry's flattened args map (dotted keys -> interpolated
// scalar strings, as nevr::PluginSpec delivers them) to a FLAT JSON object
// string. Deterministic (std::map iterates sorted); every value is a JSON string;
// an empty map yields "{}". This IS the v4 args_json contract.
std::string ArgsToJson(const std::map<std::string, std::string>& args);

// The ordered, enabled-only load plan built from a parsed config. Pure: a pure
// function of `cfg` (reads cfg.Plugins()), so the test drives it from a
// NevrConfig::LoadFromString(...) with no singleton/Windows/I-O. List order is
// load order; entries with enabled=false are dropped; `file` defaults to
// name+".dll" (already applied by the parser); `args` is serialized via ArgsToJson.
std::vector<PluginLoadItem> BuildLoadPlan(const nevr::NevrConfig& cfg);

}  // namespace nevr_plugincfg
