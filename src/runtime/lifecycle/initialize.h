#pragma once

#include "core/pch.h"

/// Set the game base address and run the full runtime initialization (resolve
/// function pointers, install hooks). The caller (nevr.dll's
/// NEVR_GetRuntimeApi::initialize) has already validated the host context and the
/// host has validated the game image; this is the runtime-side entry that
/// replaces the monolith's InitializeGameModule. Idempotent.
void InitializeValidatedRuntime(HMODULE module);

/// The window handle for the current game window (set by SetWindowTextAHook).
extern HWND g_hWindow;

/// Set to TRUE if any boot hook (MH_CreateHook / MH_EnableHook / Hooking::Attach)
/// failed during Initialize(). Checked after g_isServer is known in
/// the runtime's preprocess_after — if TRUE and server mode, ServerFatal fires.
extern bool g_bootHookFailed;
