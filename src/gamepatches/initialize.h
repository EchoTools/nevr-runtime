#pragma once

#include "common/pch.h"

/// Main initialization entry point — installs all hooks and patches.
VOID Initialize();

/// The window handle for the current game window (set by SetWindowTextAHook).
extern HWND g_hWindow;
