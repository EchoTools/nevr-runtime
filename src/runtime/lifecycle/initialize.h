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

/// The first boot failure's short cause (site and reason), set next to g_bootHookFailed so the
/// server's refusal in boot.cpp names what failed. Written from the boot sequence, which can run under
/// the DllMain loader lock, so it is a fixed buffer filled by NoteBootHookFailureCause: no heap, no Log().
extern char g_bootHookFailedCause[96];

/// Record `cause` as the reason the boot failed, unless an earlier failure already did.
inline void NoteBootHookFailureCause(const char* cause) {
  if (g_bootHookFailedCause[0] != '\0' || cause == nullptr) return;
  size_t i = 0;
  for (; i + 1 < sizeof(g_bootHookFailedCause) && cause[i] != '\0'; ++i) g_bootHookFailedCause[i] = cause[i];
  g_bootHookFailedCause[i] = '\0';
}
