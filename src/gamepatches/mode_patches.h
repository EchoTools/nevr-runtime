#pragma once

#include "common/pch.h"

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
VOID PatchServerFramePacing();
VOID PatchLogServerProfile();

// Hook installation wrappers (called from Initialize)
VOID InstallEntityHooks();
VOID InstallBugSplatHook();
VOID InstallGameSpaceHook();
VOID InstallGameMainHook();

/// N83/N84: report how many times the broadcaster hooks were ENTERED. Distinguishes
/// "guard never tripped" from "hook never ran" — only the first is evidence.
void LogBroadcasterHookStats();
