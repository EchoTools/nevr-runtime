#pragma once

#include "pch.h"

/// <summary>
/// Initializes the hash discovery hooks.
/// This implements hooks from:
/// - /home/andrew/src/evr-reconstruction/docs/usage/QUICK_START_HASHING_HOOKS.md
/// - /home/andrew/src/evr-reconstruction/docs/usage/REPLICATED_VARIABLES_HOOK_GUIDE.md
/// </summary>
VOID InitializeHashHooks();

/// <summary>
/// Shuts down the hash hooks and flushes log file.
/// </summary>
VOID ShutdownHashHooks();
