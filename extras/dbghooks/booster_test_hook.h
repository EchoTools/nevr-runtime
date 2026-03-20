#pragma once

#include <cstdint>

#include "pch.h"

// =============================================================================
// Booster Test Hook - Header
// =============================================================================
// This hook tracks booster (X key) activation as a baseline test to verify
// our hook infrastructure works before tackling weapon fire.
//
// APPROACH:
//   1. Hook CKeyboardInputDevice::InputState (same function as weapon_input_hook)
//   2. Monitor for X key (0x58) queries that return > 0.0 (pressed)
//   3. Log diagnostic info: timestamp, key state, call frequency
//   4. NO game modification - pure observation
//
// SUCCESS CRITERIA:
//   - ✅ Log shows "[BOOSTER_TEST]" when X is pressed
//   - ✅ HTTP API shows player velocity increases
//   - ✅ Timing correlation: log timestamp → API movement within 50ms
//
// If successful → Confirms hook mechanism works, apply to weapon fire
// =============================================================================

// =============================================================================
// Hook Addresses
// =============================================================================

// CKeyboardInputDevice::InputState
#define ADDR_InputState_Booster 0x0f9b330  // RVA: 0x140f9b330

// =============================================================================
// Virtual Key Codes
// =============================================================================

#define VK_X 0x58  // X key (booster)

// =============================================================================
// Function Type Definitions
// =============================================================================

// Signature: float InputState(CKeyboardInputDevice* this, uint64_t keyCode)
typedef float(__fastcall* pInputStateFuncBooster)(void* device, uint64_t keyCode);

// =============================================================================
// Public API
// =============================================================================

/// <summary>
/// Initializes the booster test hook.
/// Opens log file and installs InputState hook.
/// </summary>
VOID InitializeBoosterTestHook();

/// <summary>
/// Shuts down the booster test hook and closes log file.
/// </summary>
VOID ShutdownBoosterTestHook();
