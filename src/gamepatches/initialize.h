#pragma once

#include "common/pch.h"

/// Phase 1: DllMain-safe early init — raw byte patches only, no MinHook.
VOID InitializeEarly();

/// Phase 2: Full initialization with MinHook — call after loader lock released.
VOID Initialize();

/// The window handle for the current game window (set by SetWindowTextAHook).
extern HWND g_hWindow;
