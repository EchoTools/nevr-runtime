#pragma once

#include "common/nevr_plugin_interface.h"

// Discover and load all plugins from the plugins/ subdirectory.
// Must be called after Hooking::Initialize() and after g_isServer/g_isHeadless are known.
void LoadPlugins();

// Unload all loaded plugins (called on process detach).
void UnloadPlugins();

// Call NvrPluginOnFrame on all loaded plugins that export it.
void TickPlugins(const NvrGameContext* ctx);

// Call NvrPluginOnGameStateChange on all loaded plugins that export it.
void NotifyPluginsStateChange(const NvrGameContext* ctx, uint32_t old_state, uint32_t new_state);
