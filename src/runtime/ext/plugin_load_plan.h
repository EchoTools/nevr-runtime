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

// One configured plugin entry, in config.yaml list order. `file` is the dll
// filename to load from the plugins/ dir; `required` drives fatal-on-failure;
// `target` is a deployment-target hint (carried, unused by the loader in S6);
// `args_json` is the entry's args as a flat JSON object string ("{}" when none);
// `enabled` false means the loader skips it. Disabled entries are carried (not
// dropped) so the login can report them (#60). `required`, `enabled` and
// `args_replaced_keys` have default member initializers, so a brace initializer
// may stop after `args_json` (or after `enabled`) without a missing-initializer warning.
// `args_replaced_keys` names the arg keys whose key or value held invalid UTF-8
// and reached the plugin with U+FFFD in place of the bad bytes (never the values);
// it is empty when args_json is byte-exact.
struct PluginLoadItem {
  std::string name;
  std::string file;
  bool        required = false;
  std::string target;
  std::string args_json;
  bool        enabled = true;
  std::vector<std::string> args_replaced_keys{};
};

// The ordered plugin load plan from config.yaml's `plugins:` list: every entry,
// disabled ones included with enabled=false. Reads the same config.yaml singleton
// the rest of the runtime uses. EMPTY when no plugins are configured: config is
// authoritative and there is NO directory glob fallback (an absent/empty
// `plugins:` list loads nothing). Defined in
// service_config.cpp (which owns the singleton); the pure builder it delegates to
// is BuildLoadPlan (plugin_load_plan_build.h / .cpp).
std::vector<PluginLoadItem> NevrCfgPluginLoadPlan();

// The index of an earlier ENABLED entry of `plan` that names the same file as
// plan[i] (compared without case, as Windows compares file names), or -1 when
// there is none. LoadLibrary hands back the already-loaded module for such a
// repeat, so loading it would run the plugin's init twice and deliver every
// OnFrame / state change to it twice; the loader skips it instead. A disabled
// earlier entry doesn't count: it was never loaded.
inline long DuplicatePluginEntry(const std::vector<PluginLoadItem>& plan, size_t i) {
  auto same = [](const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    for (size_t k = 0; k < a.size(); ++k) {
      char x = a[k], y = b[k];
      if (x >= 'A' && x <= 'Z') x = static_cast<char>(x - 'A' + 'a');
      if (y >= 'A' && y <= 'Z') y = static_cast<char>(y - 'A' + 'a');
      if (x != y) return false;
    }
    return true;
  };
  if (i >= plan.size()) return -1;
  for (size_t j = 0; j < i; ++j) {
    if (plan[j].enabled && same(plan[j].file, plan[i].file)) return static_cast<long>(j);
  }
  return -1;
}

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
