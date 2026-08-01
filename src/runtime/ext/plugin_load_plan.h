// plugin_load_plan.h — the PCH-safe surface the plugin loader consumes (N134 S6).
//
// The loader (plugin_loader.cpp) is compiled WITH the core PCH (windows.h without
// NOMINMAX). It must therefore NOT reach nevr_config.h (whose <optional>/<map>
// trip the min/max macros). So this header exposes ONLY std::string/std::vector/
// bool — the resolved load plan and the init-export choice — and the yaml-facing
// half (BuildLoadPlan over nevr::PluginSpec, ArgsToJson) lives in the SKIP_PCH
// header plugin_load_plan_build.h, included only by the pure impl, the config
// singleton bridge, and the test. Mirrors the service_map (pure, PCH-safe) /
// service_config (impure singleton) split from N133 S3.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

// One resolved, ENABLED plugin to load, in config.yaml list order. `file` is the
// dll filename to load from the plugins/ dir; `required` drives fatal-on-failure;
// `target` is a deployment-target hint (carried, unused by the loader in S6);
// `args_json` is the entry's args as a flat JSON object string ("{}" when none).
struct PluginLoadItem {
  std::string name;
  std::string file;
  bool        required = false;
  std::string target;
  std::string args_json;
};

// The ordered, enabled-only plugin load plan from config.yaml's `plugins:` list.
// Reads the same config.yaml singleton the rest of the runtime uses. EMPTY when
// no plugins are configured: config is authoritative and there is NO directory
// glob fallback (an absent/empty `plugins:` list loads nothing). Defined in
// service_config.cpp (which owns the singleton); the pure builder it delegates to
// is BuildLoadPlan (plugin_load_plan_build.h / .cpp).
std::vector<PluginLoadItem> NevrCfgPluginLoadPlan();

// Which init export the loader should call for a plugin, given which exports it
// resolved. Prefers the v4 args-aware NvrPluginInitEx; falls back to the v3
// NvrPluginInit (so a v3 plugin still loads, without args); None when the plugin
// exports neither (init is optional — the plugin still loads). Pure + inline so
// both the loader and the test share ONE definition with no extra link symbol.
enum class PluginInitKind : std::uint8_t { None = 0, Legacy = 1, Ex = 2 };

inline PluginInitKind ChoosePluginInit(bool hasInitEx, bool hasInit) {
  if (hasInitEx) return PluginInitKind::Ex;   // v4 preferred (receives args_json)
  if (hasInit)   return PluginInitKind::Legacy;  // v3 fallback (no args)
  return PluginInitKind::None;                 // neither — plugin loads, no init
}
