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
VOID PatchDisableServerRendering(PVOID pGame);
VOID PatchLogServerProfile();

// Hook installation wrappers (called from Initialize)
VOID InstallEntityHooks();
VOID InstallBugSplatHook();
VOID InstallGameSpaceHook();
VOID InstallGameMainHook();
