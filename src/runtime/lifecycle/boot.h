#pragma once

#include "core/pch.h"

/// <summary>
/// A detour hook for the game's command line pre-processing method, used to parse command line arguments.
/// </summary>
/// <param name="pGame">A pointer to the game instance.</param>
UINT64 PreprocessCommandLineHook(PVOID pGame);

/// Complete NEVR startup after the game has initialized its graphics path.
///
/// The client invokes this after D3D11/D3D12 device creation returns; the
/// Preprocess hook invokes it on its second pass for the dedicated server,
/// which does not create a graphics device. The function is idempotent.
void RunDeferredRuntimeBootstrap(PVOID pGame, const char* trigger);
