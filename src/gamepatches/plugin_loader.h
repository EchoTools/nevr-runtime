#pragma once

#include "common/plugin_api.h"

// Discover and load all plugins from the plugins/ subdirectory.
// Must be called after Hooking::Initialize() and after g_isServer/g_isHeadless are known.
// In practice this means it runs from PreprocessCommandLineHook (after CLI parsing),
// not from Initialize() (which runs before CLI parsing).
void LoadPlugins();

// Unload all loaded plugins (called on process detach).
void UnloadPlugins();
