#pragma once

#include "common/pch.h"

/// <summary>
/// A detour hook for the game's command line pre-processing method, used to parse command line arguments.
/// </summary>
/// <param name="pGame">A pointer to the game instance.</param>
UINT64 PreprocessCommandLineHook(PVOID pGame);
