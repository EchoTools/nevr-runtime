#pragma once

#include "core/pch.h"

/// Validate the game image before assigning its base or initializing any hooks.
void InitializeGameModule(HMODULE module);

/// The window handle for the current game window (set by SetWindowTextAHook).
extern HWND g_hWindow;

/// Set to TRUE if any boot hook (MH_CreateHook / MH_EnableHook / Hooking::Attach)
/// failed during Initialize(). Checked after g_isServer is known in
/// PreprocessCommandLineHook — if TRUE and server mode, ServerFatal fires.
extern bool g_bootHookFailed;
