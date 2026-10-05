#pragma once

#include <string>

#include "extension/plugin_interface.h"

// Discover and load all plugins from the plugins/ subdirectory.
// Must be called after Hooking::Initialize() and after g_isServer/g_isHeadless are known.
void LoadPlugins();

// Explicit normal-thread teardown helper: calls each optional shutdown export
// in reverse load order, then releases the host's module reference. DllMain
// does not call this; shutdown is not guaranteed on process exit, and runtime
// dynamic unloading is not a supported lifecycle path.
void UnloadPlugins();

// Call NvrPluginOnFrame on all loaded plugins that export it.
void TickPlugins(const NvrGameContext* ctx);

// Call NvrPluginOnGameStateChange on all loaded plugins that export it.
void NotifyPluginsStateChange(const NvrGameContext* ctx, uint32_t old_state, uint32_t new_state);

// Plugin-query API (N134 S8, v5). A plugin calls ctx->get_plugin_count() and
// ctx->get_plugin_info(n) to discover its neighbours at runtime. These are
// filled into every NvrGameContext by the host (LoadPlugins / TickPlugins /
// NotifyPluginsStateChange), so a v5+ plugin that checks ctx->ctx_size can
// call them from its init, on_frame, or on_state_change callback.
int  GetLoadedPluginCount(void);
const NvrLoadedPluginInfo* GetLoadedPluginInfo(int index);

// N134 S8: caps-based load-order priority. Lower loads first. This is the
// sort key consumed by the loader's stable_sort in LoadPlugins. Defined
// inline so both the loader and the test share one truth.
inline constexpr int CapsLoadPriority(uint32_t caps) {
  if (caps == NEVR_PLUGIN_CAP_UNDECLARED) return 0;
  // Bands, lowest-first:
  //   0: UNDECLARED (unknown risk — load earliest, let declared plugins land
  //      on top where HookGuard catches collisions)
  //   1: OBSERVES_ONLY (reads state, never writes)
  //   2: COSMETIC (visuals/audio only)
  //   3: ALTERS_GAMEPLAY (physics, weapons, movement)
  //   4: ALTERS_RULES (rules, scoring, game mode itself)
  //   5: NETWORK (external sockets)
  //   6: HOOKS_ENGINE (own detours — load LAST)
  if (caps & NEVR_PLUGIN_CAP_HOOKS_ENGINE)     return 6;
  if (caps & NEVR_PLUGIN_CAP_ALTERS_RULES)     return 4;
  if (caps & NEVR_PLUGIN_CAP_NETWORK)          return 5;
  if (caps & NEVR_PLUGIN_CAP_ALTERS_GAMEPLAY)  return 3;
  if (caps & NEVR_PLUGIN_CAP_COSMETIC)         return 2;
  if (caps & NEVR_PLUGIN_CAP_OBSERVES_ONLY)    return 1;
  return 0;
}

// N112 / #60 — the JSON array the client login sends as `nevr_plugins`: one
// entry per entry of config.yaml's `plugins:` list, in list order, recording
// what the last LoadPlugins() did with it. Shape (see plugin_manifest.h):
//   [{"name":"ex","file":"ex.dll","enabled":true,"required":false,"loaded":true,
//     "ver":"1.0.0","api":5,"caps":1},
//    {"name":"gate","file":"gate.dll","enabled":true,"required":true,"loaded":false,
//     "error":"LoadLibrary failed: error 126"},
//    {"name":"off","file":"off.dll","enabled":false,"required":false,"loaded":false}]
// Returns "[]" before LoadPlugins(), after UnloadPlugins(), or when no plugins
// are configured. Only the client login sends it; server registration does not.
std::string BuildPluginManifestJson();

// ============================================================================
// Test hooks — enabled only when NEVR_TEST_HOOKS is defined.
// Allow unit tests to inject mock callbacks into the plugin registry
// so TickPlugins / NotifyPluginsStateChange behavior can be verified.
// ============================================================================

#ifdef NEVR_TEST_HOOKS
void TestHook_RegisterPluginOnFrame(NvrPluginOnFrame_fn fn);
void TestHook_RegisterPluginOnStateChange(NvrPluginOnGameStateChange_fn fn);
void TestHook_ClearPlugins();
#endif
