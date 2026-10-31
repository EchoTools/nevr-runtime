#pragma once

#include "core/pch.h"
#include "runtime/patch/checked_patch.h"

// ============================================================================
// Memory patches and optimization hooks
// ============================================================================

VOID PatchEnableHeadless(PVOID pGame);
VOID PatchBypassOvrPlatform();
VOID PatchEnableServer();
VOID PatchEnableOffline();
VOID PatchNoOvrRequiresSpectatorStream();
VOID PatchDeadlockMonitor();
VOID PatchBlockOculusSDK();
VOID PatchDisableWwise();
VOID PatchLogServerProfile();
/// Validate and NOP the -spectatorstream check inside PreprocessCommandLine; one record per call.
/// Idempotent: a second call reports already_applied and writes nothing. (#519)
nevr_checked_patch::Result PatchSpectatorStreamCheck(const char* phase);

// Hook installation wrappers (called from Initialize)
VOID InstallEntityHooks();
VOID InstallBugSplatHook();
VOID InstallGameSpaceHook();
// RuntimeGameMain and ArmGameMain are in lifecycle/crash_recovery.h (N125) — they carry the
// setjmp side of the game-loop recovery whose longjmp lives in crash_recovery.cpp.

