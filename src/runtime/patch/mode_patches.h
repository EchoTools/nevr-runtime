#pragma once

#include "core/pch.h"

// ============================================================================
// Memory patches and optimization hooks
// ============================================================================

VOID PatchEnableHeadless(PVOID pGame);
VOID PatchBypassOvrPlatform();
VOID PatchDisableLoadingTips();
VOID PatchEnableServer();
VOID PatchEnableOffline();
VOID PatchNoOvrRequiresSpectatorStream();
VOID PatchDeadlockMonitor();
VOID PatchBlockOculusSDK();
VOID PatchDisableWwise();
VOID PatchLogServerProfile();
VOID PatchSpectatorStreamAlways();

// Hook installation wrappers (called from Initialize)
VOID InstallEntityHooks();
VOID InstallBugSplatHook();
VOID InstallGameSpaceHook();
// InstallGameMainHook moved to lifecycle/crash_recovery.h (N125) — it installs the
// setjmp side of the game-loop recovery whose longjmp lives in crash_recovery.cpp.

