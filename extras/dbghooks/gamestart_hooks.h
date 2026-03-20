#pragma once

#include "pch.h"

/// <summary>
/// Initializes game start debugging hooks to investigate the bit 43 trigger.
/// 
/// Background:
/// - Clients stuck on black loading screen waiting for state 8 → 9 transition
/// - Binary analysis shows transition requires bit 43 set at NetGame+0x2da0
/// - Custom servers don't set this bit, causing soft-lock
/// 
/// Hooks installed:
/// 1. State transition function @ 0x1401bc800 - logs state changes and bit 43 status
/// 
/// Output: gamestart_debug.log in game directory
/// </summary>
VOID InitializeGameStartHooks();

/// <summary>
/// Shuts down game start hooks and flushes log file.
/// </summary>
VOID ShutdownGameStartHooks();
