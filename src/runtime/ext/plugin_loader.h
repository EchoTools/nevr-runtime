#pragma once

#include "extension/plugin_interface.h"

// Discover and load all plugins from the plugins/ subdirectory.
// Must be called after Hooking::Initialize() and after g_isServer/g_isHeadless are known.
void LoadPlugins();

// Unload all loaded plugins (called on process detach).
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
