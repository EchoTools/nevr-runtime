#pragma once

#include "core/pch.h"

/// Main initialization entry point — installs all hooks and patches.
VOID Initialize();

/// The window handle for the current game window (set by SetWindowTextAHook).
extern HWND g_hWindow;

/// Set to TRUE if any boot hook (MH_CreateHook / MH_EnableHook / Hooking::Attach)
/// failed during Initialize(). Checked after g_isServer is known in
/// PreprocessCommandLineHook — if TRUE and server mode, ServerFatal fires.
extern bool g_bootHookFailed;
