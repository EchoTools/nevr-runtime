#pragma once

#include <string>

#include "extension/plugin_interface.h"
#include "runtime/ext/plugin_load_plan.h"  // PluginPhase

// Load the plugins config.yaml lists for `phase` from the plugins/ subdirectory:
// the entries marked `early: true` in the early pass, the rest in the normal one
// (see PluginPhase). An early pass with no entry marked early loads nothing.
// Must be called after Hooking::Initialize() and after g_isServer/g_isHeadless are known.
void LoadPlugins(PluginPhase phase = PluginPhase::Normal);

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

// #598: ctx->log. The host writes a plugin's line through Log() as
// "[NEVR.PLUGIN] plugin=<id> <message> <key>=<value> ..." (see plugin_log.h). Every context the loader
// passes a plugin (init, per-frame, state change) carries the plugin's id and this function, so the
// caller's own context needs neither.
void HostPluginLog(const char* pluginId, uint32_t level, const char* message,
                   const NevrLogField* fields, uint32_t fieldCount);

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
// #741: whether LoadModule may retry with the default search order after the restricted load failed.
bool TestHook_RestrictedSearchFallbackAllowed(bool enforcing, unsigned long error);
// #741: the refusal dialog (replaced by `sink` so a test reads the text), the refusals it names, and the filter on
// loads a plugin makes after init (registered with DllLoadHook; reset empties its directory so it does nothing).
void TestHook_SetRefusalDialogSink(void (*sink)(const std::string& text));
void TestHook_ResetRefusals();
void TestHook_ShowRefusalNoticeIfDue();
void TestHook_RecordRefusal(const std::string& file);
void TestHook_InstallPluginLoadFilter(const std::string& pluginDir);
void TestHook_ResetPluginLoadFilter();
void TestHook_ForgetLoadFilterRegistration();  // after nevr_dll_load_hook::Shutdown() cleared the registered filters
// How many files the late-load filter has checked and holds (a staged plugin's own load is not one of them).
size_t TestHook_LoadFilterVerifiedCount();
// Called by the filter between resolving a late load's path and opening it (null clears it).
void TestHook_SetBetweenResolveAndOpen(void (*hook)(const std::wstring& resolved));
// Called by StageOne between a staged plugin's check and its load, with the path the entry names (null clears it).
void TestHook_SetBetweenCheckAndLoad(void (*hook)(const std::wstring& staged));
// Makes the query for the path of the file a staged plugin's check holds fail.
void TestHook_FailNamingTheHeldFile(bool fail);
// Called after a plugin (a staged one, or a later load the filter accepted) is checked and before the load that maps it (null clears it).
void TestHook_SetAfterDependenciesChecked(void (*hook)());
// Makes every System32 pin of a plugin's dependencies fail with ERROR_ACCESS_DENIED (false restores it).
void TestHook_FailPinning(bool fail);
// Runs after a late load's check has passed and before the filter lets the load go on (#1177).
void TestHook_SetAfterLateLoadAccepted(void (*hook)(const std::wstring& checked));
#endif
