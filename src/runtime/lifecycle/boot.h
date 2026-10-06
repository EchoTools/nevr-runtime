#pragma once

#include "core/pch.h"

/// Parse the command line for the minimal state the runtime needs BEFORE the
/// game's original PreprocessCommandLine runs: -server, -windowed, -config/
/// -config-path. Called by nevr.dll's preprocess_before dispatch (the host owns
/// the PreprocessCommandLine detour and calls the original itself). Idempotent.
void PreflightRuntimeBootstrap();

/// Complete NEVR startup after the game has initialized its graphics path.
///
/// The client invokes this after D3D11/D3D12 device creation returns; the
/// Preprocess hook invokes it on its first call for the dedicated server,
/// which does not create a graphics device. The function is idempotent.
void RunDeferredRuntimeBootstrap(PVOID pGame, const char* trigger);
